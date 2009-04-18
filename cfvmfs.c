/*
 * VMFS prototype (based on http://code.google.com/p/vmfs/ from fluidOps)
 * C.Fillot, 2009/04/15
 */

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <uuid/uuid.h>

#include "utils.h"
#include "vmfs.h"

/* VMFS meta-files */
#define VMFS_FBB_FILENAME  ".fbb.sf"
#define VMFS_FDC_FILENAME  ".fdc.sf"
#define VMFS_PBC_FILENAME  ".pbc.sf"
#define VMFS_SBC_FILENAME  ".sbc.sf"
#define VMFS_VH_FILENAME   ".vh.sf"

#define VMFS_FDC_BASE       0x1400000

/* === Volume Info === */
#define VMFS_VOLINFO_BASE   0x100000
#define VMFS_VOLINFO_MAGIC  0xc001d00d

#define VMFS_VOLINFO_OFS_MAGIC 0x0000
#define VMFS_VOLINFO_OFS_VER   0x0004
#define VMFS_VOLINFO_OFS_NAME  0x0012
#define VMFS_VOLINFO_OFS_UUID  0x0082 
#define VMFS_VOLINFO_OFS_SIZE  0x0200
#define VMFS_VOLINFO_OFS_BLKS  0x0208

#define VMFS_VOLINFO_OFS_NAME_SIZE     28

struct vmfs_volinfo {
   m_u32_t magic;
   m_u32_t version;
   char *name;
   uuid_t uuid;

   m_u64_t size;
   m_u64_t blocks;
};

/* === FS Info === */
#define VMFS_FSINFO_BASE   0x1200000
#define VMFS_FSINFO_MAGIC  0x2fabf15e

#define VMFS_FSINFO_OFS_MAGIC    0x0000
#define VMFS_FSINFO_OFS_VOLVER   0x0004
#define VMFS_FSINFO_OFS_VER      0x0008
#define VMFS_FSINFO_OFS_UUID     0x0009
#define VMFS_FSINFO_OFS_LABEL    0x001d
#define VMFS_FSINFO_OFS_BLKSIZE  0x00a1

struct vmfs_fsinfo {
   m_u32_t magic;
   m_u32_t vol_version;
   m_u32_t version;
   uuid_t uuid;
   char label[128];

   m_u64_t block_size;
   uuid_t vol_uuid;
};

/* === Heartbeats === */
#define VMFS_HB_BASE  0x1300000

#define VMFS_HB_SIZE  0x200

#define VMFS_HB_MAGIC_OFF   0xabcdef01
#define VMFS_HB_MAGIC_ON    0xabcdef02

#define VMFS_HB_OFS_MAGIC   0x0000
#define VMFS_HB_OFS_POS     0x0004
#define VMFS_HB_OFS_UPTIME  0x0014
#define VMFS_HB_OFS_UUID    0x001c

struct vmfs_heartbeat {
   m_u32_t magic;
   m_u64_t position;
   m_u64_t uptime;       /* Uptime (in usec) of the locker */
   uuid_t uuid;          /* UUID of the server */
};

/* === File Meta Info === */
#define VMFS_FILE_INFO_SIZE  0x800

#define VMFS_FILEINFO_OFS_GRP_ID     0x0000
#define VMFS_FILEINFO_OFS_POS        0x0004
#define VMFS_FILEINFO_OFS_HB_POS     0x000c
#define VMFS_FILEINFO_OFS_HB_LOCK    0x0024
#define VMFS_FILEINFO_OFS_HB_UUID    0x0028
#define VMFS_FILEINFO_OFS_ID         0x0200
#define VMFS_FILEINFO_OFS_ID2        0x0204
#define VMFS_FILEINFO_OFS_TYPE       0x020c
#define VMFS_FILEINFO_OFS_SIZE       0x0214
#define VMFS_FILEINFO_OFS_TS1        0x022c
#define VMFS_FILEINFO_OFS_TS2        0x0230
#define VMFS_FILEINFO_OFS_TS3        0x0234
#define VMFS_FILEINFO_OFS_UID        0x0238
#define VMFS_FILEINFO_OFS_GID        0x023c
#define VMFS_FILEINFO_OFS_MODE       0x0240

#define VMFS_FILEINFO_OFS_BLK_ARRAY  0x0400
#define VMFS_FILEINFO_BLK_COUNT      0x100

struct vmfs_file_info {
   m_u32_t group_id;
   m_u64_t position;
   m_u64_t hb_pos;
   m_u32_t hb_lock;
   uuid_t  hb_uuid;
   m_u32_t id,id2;
   m_u32_t type;
   m_u64_t size;
   m_u32_t ts1,ts2,ts3;
   m_u32_t uid,gid;
   m_u32_t mode;
};

/* === File Record === */
#define VMFS_FILE_RECORD_SIZE    0x8c

#define VMFS_FILEREC_OFS_TYPE    0x0000
#define VMFS_FILEREC_OFS_BLK_ID  0x0004
#define VMFS_FILEREC_OFS_REC_ID  0x0008
#define VMFS_FILEREC_OFS_NAME    0x000c

struct vmfs_file_record {
   m_u32_t type;
   m_u32_t block_id;
   m_u32_t record_id;
   char name[128];
};

/* === VMFS file abstraction === */
struct vmfs_file {
   vmfs_volume_t *vol;
   vmfs_blk_list_t blk_list;
   vmfs_file_info_t file_info;

   /* Current position in file */
   off_t pos;

   /* ... */
};

/* === VMFS mounted-volume === */
struct vmfs_volume {
   char *filename;
   FILE *fd;
   int debug_level;

   /* VMFS volume base */
   off_t vmfs_base;

   /* FDC base */
   off_t fdc_base;

   /* Volume and FS information */
   vmfs_volinfo_t vol_info;
   vmfs_fsinfo_t fs_info;

   /* Meta-files containing file system structures */
   vmfs_file_t *fbb,*fdc,*pbc,*sbc,*vh,*root_dir;

   /* Bitmap headers of meta-files */
   vmfs_bitmap_header_t fbb_bmh,fdc_bmh,pbc_bmh,sbc_bmh;
};

/* Forward declarations */
static inline m_u64_t vmfs_vol_get_blocksize(vmfs_volume_t *vol);
ssize_t vmfs_vol_read_data(vmfs_volume_t *vol,off_t pos,u_char *buf,size_t len);
ssize_t vmfs_vol_read(vmfs_volume_t *vol,m_u32_t blk,off_t offset,
                      u_char *buf,size_t len);
ssize_t vmfs_file_read(vmfs_file_t *f,u_char *buf,size_t len);
int vmfs_file_seek(vmfs_file_t *f,off_t pos,int whence);
static vmfs_file_t *
vmfs_file_open_rec(vmfs_volume_t *vol,vmfs_file_record_t *rec);

/* ======================================================================== */
/* Marshalling                                                              */
/* ======================================================================== */

/* Read volume information */
int vmfs_volinfo_read(vmfs_volinfo_t *vol,FILE *fd)
{
   u_char buf[1024];

   if (fseeko(fd,VMFS_VOLINFO_BASE,SEEK_SET) != 0)
      return(-1);

   if (fread(buf,sizeof(buf),1,fd) != 1)
      return(-1);

   vol->magic   = read_le32(buf,VMFS_VOLINFO_OFS_MAGIC);
   vol->version = read_le32(buf,VMFS_VOLINFO_OFS_VER);
   vol->size    = read_le64(buf,VMFS_VOLINFO_OFS_SIZE);
   vol->blocks  = read_le64(buf,VMFS_VOLINFO_OFS_BLKS);

   vol->name = strndup((char *)buf+VMFS_VOLINFO_OFS_NAME, VMFS_VOLINFO_OFS_NAME_SIZE);
   memcpy(vol->uuid,buf+VMFS_VOLINFO_OFS_UUID,sizeof(vol->uuid));

   if (vol->magic != VMFS_VOLINFO_MAGIC) {
      fprintf(stderr,"VMFS VolInfo: invalid magic number 0x%8.8x\n",vol->magic);
      return(-1);
   }

   return(0);
}

/* Show volume information */
void vmfs_volinfo_show(vmfs_volinfo_t *vol)
{
   char uuid_str[M_UUID_BUFLEN];

   printf("VMFS Volume Information:\n");
   printf("  - Version : %d\n",vol->version);
   printf("  - Name    : %s\n",vol->name);
   printf("  - UUID    : %s\n",m_uuid_to_str(vol->uuid,uuid_str));
   printf("  - Size    : %llu Gb\n",vol->size / (1024*1048576));
   printf("  - Blocks  : %llu\n",vol->blocks);


   printf("\n");
}

/* Read filesystem information */
int vmfs_fsinfo_read(vmfs_fsinfo_t *fsi,FILE *fd,off_t base)
{
   u_char buf[512];

   if (fseek(fd,base+VMFS_FSINFO_BASE,SEEK_SET) != 0)
      return(-1);

   if (fread(buf,sizeof(buf),1,fd) != 1)
      return(-1);

   fsi->magic       = read_le32(buf,VMFS_FSINFO_OFS_MAGIC);
   fsi->vol_version = read_le32(buf,VMFS_FSINFO_OFS_VOLVER);
   fsi->version     = buf[VMFS_FSINFO_OFS_VER];

   fsi->block_size  = read_le32(buf,VMFS_FSINFO_OFS_BLKSIZE);

   memcpy(fsi->uuid,buf+VMFS_FSINFO_OFS_UUID,sizeof(fsi->uuid));
   memcpy(fsi->label,buf+VMFS_FSINFO_OFS_LABEL,sizeof(fsi->label));

   if (fsi->magic != VMFS_FSINFO_MAGIC) {
      fprintf(stderr,"VMFS FSInfo: invalid magic number 0x%8.8x\n",fsi->magic);
      return(-1);
   }

   return(0);
}

/* Show FS information */
void vmfs_fsinfo_show(vmfs_fsinfo_t *fsi)
{  
   char uuid_str[M_UUID_BUFLEN];

   printf("VMFS FS Information:\n");

   printf("  - Vol. Version : %d\n",fsi->vol_version);
   printf("  - Version      : %d\n",fsi->version);
   printf("  - Label        : %s\n",fsi->label);
   printf("  - UUID         : %s\n",m_uuid_to_str(fsi->uuid,uuid_str));
   printf("  - Block size   : %llu (0x%llx)\n",
          fsi->block_size,fsi->block_size);

   printf("\n");
}

/* Read a heartbeart info */
int vmfs_heartbeat_read(vmfs_heartbeat_t *hb,u_char *buf)
{
   hb->magic    = read_le32(buf,VMFS_HB_OFS_MAGIC);
   hb->position = read_le64(buf,VMFS_HB_OFS_POS);
   hb->uptime   = read_le64(buf,VMFS_HB_OFS_UPTIME);
   memcpy(hb->uuid,buf+VMFS_HB_OFS_UUID,sizeof(hb->uuid));

   return(0);
}

/* Show heartbeat info */
void vmfs_heartbeat_show(vmfs_heartbeat_t *hb)
{
   char uuid_str[M_UUID_BUFLEN];
   
   printf("Heartbeat ID 0x%llx:\n",hb->position);

   printf("  - Magic  : 0x%8.8x\n",hb->magic);
   printf("  - Uptime : 0x%8.8llx\n",hb->uptime);
   printf("  - UUID   : %s\n",m_uuid_to_str(hb->uuid,uuid_str));

   printf("\n");
}

/* Read a file meta info */
int vmfs_fmi_read(vmfs_file_info_t *fmi,u_char *buf)
{
   fmi->group_id = read_le32(buf,VMFS_FILEINFO_OFS_GRP_ID);
   fmi->position = read_le64(buf,VMFS_FILEINFO_OFS_POS);
   fmi->hb_pos   = read_le64(buf,VMFS_FILEINFO_OFS_HB_POS);
   fmi->hb_lock  = read_le32(buf,VMFS_FILEINFO_OFS_HB_LOCK);
   fmi->id       = read_le32(buf,VMFS_FILEINFO_OFS_ID);
   fmi->id2      = read_le32(buf,VMFS_FILEINFO_OFS_ID2);
   fmi->type     = read_le32(buf,VMFS_FILEINFO_OFS_TYPE);
   fmi->size     = read_le64(buf,VMFS_FILEINFO_OFS_SIZE);
   fmi->ts1      = read_le32(buf,VMFS_FILEINFO_OFS_TS1);
   fmi->ts2      = read_le32(buf,VMFS_FILEINFO_OFS_TS2);
   fmi->ts3      = read_le32(buf,VMFS_FILEINFO_OFS_TS3);
   fmi->uid      = read_le32(buf,VMFS_FILEINFO_OFS_UID);
   fmi->gid      = read_le32(buf,VMFS_FILEINFO_OFS_GID);
   fmi->mode     = read_le32(buf,VMFS_FILEINFO_OFS_MODE);

   memcpy(fmi->hb_uuid,buf+VMFS_FILEINFO_OFS_HB_UUID,sizeof(fmi->hb_uuid));
   return(0);
}

/* Read a file descriptor */
int vmfs_frec_read(vmfs_file_record_t *frec,u_char *buf)
{
   frec->type      = read_le32(buf,VMFS_FILEREC_OFS_TYPE);
   frec->block_id  = read_le32(buf,VMFS_FILEREC_OFS_BLK_ID);
   frec->record_id = read_le32(buf,VMFS_FILEREC_OFS_REC_ID);
   memcpy(frec->name,buf+VMFS_FILEREC_OFS_NAME,sizeof(frec->name));
   return(0);
}

/* ======================================================================== */
/* Heartbeats                                                               */
/* ======================================================================== */

int vmfs_heartbeat_show_active(vmfs_volume_t *vol)
{
   u_char buf[VMFS_HB_SIZE];
   vmfs_heartbeat_t hb;
   ssize_t res;
   off_t pos = 0;
   int count = 0;

   while(pos < vmfs_vol_get_blocksize(vol)) {
      res = vmfs_vol_read(vol,3,pos,buf,sizeof(buf));

      if (res != sizeof(buf)) {
         fprintf(stderr,"VMFS: unable to read heartbeat info.\n");
         return(-1);
      }

      vmfs_heartbeat_read(&hb,buf);
      
      if (hb.magic == VMFS_HB_MAGIC_ON) {
         vmfs_heartbeat_show(&hb);
         count++;
      }

      pos += res;
   }
   
   return(count);
}

/* ======================================================================== */
/* File abstraction                                                         */
/* ======================================================================== */

/* Create a file structure */
static vmfs_file_t *vmfs_file_create_struct(vmfs_volume_t *vol)
{
   vmfs_file_t *f;

   if (!(f = calloc(1,sizeof(*f))))
      return NULL;

   f->vol = vol;
   vmfs_blk_list_init(&f->blk_list);
   return f;
}

/* Get file size */
static inline m_u64_t vmfs_file_get_size(vmfs_file_t *f)
{
   return(f->file_info.size);
}

/* Set position */
int vmfs_file_seek(vmfs_file_t *f,off_t pos,int whence)
{
   switch(whence) {
      case SEEK_SET:
         f->pos = pos;
         break;
      case SEEK_CUR:
         f->pos += pos;
         break;
      case SEEK_END:
         f->pos -= pos;
         break;
   }
   
   /* Normalize */
   if (f->pos < 0)
      f->pos = 0;
   else {
      if (f->pos > f->file_info.size)
         f->pos = f->file_info.size;
   }

   return(0);
}

/* Read data from a file */
ssize_t vmfs_file_read(vmfs_file_t *f,u_char *buf,size_t len)
{
   vmfs_bitmap_header_t *sbc_bmh;
   vmfs_file_t *sbc;
   u_int blk_pos;
   m_u32_t blk_id,blk_type;
   m_u64_t blk_size,blk_len;
   m_u64_t file_size,offset;
   ssize_t res,rlen = 0;
   size_t clen,exp_len;

   blk_size = vmfs_vol_get_blocksize(f->vol);
   file_size = vmfs_file_get_size(f);

   sbc = f->vol->sbc;
   sbc_bmh = &f->vol->sbc_bmh;

   while(len > 0) {
      blk_pos = f->pos / blk_size;

      if (vmfs_blk_list_get_block(&f->blk_list,blk_pos,&blk_id) == -1)
         break;

#if 0
      if (f->vol->debug_level > 1)
         printf("vmfs_file_read: reading block 0x%8.8x\n",blk_id);
#endif

      blk_type = VMFS_BLK_TYPE(blk_id);

      switch(blk_type) {
         /* Full-Block */
         case VMFS_BLK_TYPE_FB:
            offset = f->pos % blk_size;
            blk_len = blk_size - offset;
            exp_len = m_min(blk_len,len);
            clen = m_min(exp_len,file_size - f->pos);

#if 0
            printf("vmfs_file_read: f->pos=0x%llx, offset=0x%8.8llx\n",
                   (m_u64_t)f->pos,offset);
#endif

            res = vmfs_vol_read(f->vol,VMFS_BLK_FB_NUMBER(blk_id),offset,
                                buf,clen);
            break;

         /* Sub-Block */
         case VMFS_BLK_TYPE_SB: {
            m_u32_t sbc_subgroup,sbc_number,sbc_blk;
            off_t sbc_addr;

            offset = f->pos % sbc_bmh->data_size;
            blk_len = sbc_bmh->data_size - offset;
            exp_len = m_min(blk_len,len);
            clen = m_min(exp_len,file_size - f->pos);

            sbc_subgroup = VMFS_BLK_SB_SUBGROUP(blk_id);
            sbc_number   = VMFS_BLK_SB_NUMBER(blk_id);

            sbc_blk = sbc_number * sbc_bmh->items_per_bitmap_entry;
            sbc_blk += sbc_subgroup;

            sbc_addr = vmfs_bitmap_get_block_addr(sbc_bmh,sbc_blk);
            sbc_addr += offset;

            vmfs_file_seek(sbc,sbc_addr,SEEK_SET);
            res = vmfs_file_read(sbc,buf,clen);

            break;
         }

         default:
            fprintf(stderr,"VMFS: unknown block type 0x%2.2x\n",blk_type);
            return(-1);
      }

      /* Move file position and keep track of bytes currently read */
      f->pos += res;
      rlen += res;

      /* Move buffer position */
      buf += res;
      len -= res;

      /* Incomplete read, stop now */
      if (res < exp_len)
         break;
   }

   return(rlen);
}

/* Get the offset corresponding to a file meta-info in FDC file */
static inline off_t 
vmfs_get_meta_info_offset(vmfs_volume_t *vol,m_u32_t blk_id)
{
   m_u32_t subgroup,number;
   off_t fmi_addr;
   m_u32_t fdc_blk;

   subgroup = VMFS_BLK_FD_SUBGROUP(blk_id);
   number   = VMFS_BLK_FD_NUMBER(blk_id);

   /* Compute the address of the file meta-info in the FDC file */
   fdc_blk = subgroup * vol->fdc_bmh.items_per_bitmap_entry;
   fmi_addr  = vmfs_bitmap_get_block_addr(&vol->fdc_bmh,fdc_blk);
   fmi_addr += number * vol->fdc_bmh.data_size;

   return(fmi_addr);
}

/* Get the meta-file info associated to a file record */
static int vmfs_get_meta_info(vmfs_volume_t *vol,vmfs_file_record_t *rec,
                              u_char *buf)
{
   m_u32_t blk_id = rec->block_id;
   off_t fmi_addr;
   ssize_t len;

   if (VMFS_BLK_TYPE(blk_id) != VMFS_BLK_TYPE_FD)
      return(-1);

   fmi_addr = vmfs_get_meta_info_offset(vol,blk_id);

   if (vmfs_file_seek(vol->fdc,fmi_addr,SEEK_SET) == -1)
      return(-1);
   
   len = vmfs_file_read(vol->fdc,buf,vol->fdc_bmh.data_size);
   return((len == vol->fdc_bmh.data_size) ? 0 : -1);
}

/* Search for an entry into a directory */
static int vmfs_file_searchdir(vmfs_file_t *dir_entry,char *name,
                               vmfs_file_record_t *rec)
{
   u_char buf[VMFS_FILE_RECORD_SIZE];
   int dir_count;
   ssize_t len;

   dir_count = vmfs_file_get_size(dir_entry) / VMFS_FILE_RECORD_SIZE;
   vmfs_file_seek(dir_entry,0,SEEK_SET);
   
   while(dir_count > 0) {
      len = vmfs_file_read(dir_entry,buf,sizeof(buf));

      if (len != VMFS_FILE_RECORD_SIZE)
         return(-1);

      vmfs_frec_read(rec,buf);

      if (!strcmp(rec->name,name))
         return(1);
   }

   return(0);
}

/* Resolve a path name to a file record */
static int vmfs_resolve_path(vmfs_volume_t *vol,char *name,
                             vmfs_file_record_t *rec)
{
   vmfs_file_t *cur_dir,*sub_dir;
   char *ptr,*sl;

   cur_dir = vol->root_dir;
   ptr = name;
   
   for(;;) {
      sl = strchr(ptr,'/');

      if (sl != NULL)
         *sl = 0;

      if (*ptr == 0) {
         ptr = sl + 1;
         continue;
      }
             
      if (vmfs_file_searchdir(cur_dir,ptr,rec) != 1)
         return(-1);
      
      /* last token */
      if (sl == NULL)
         return(1);

      if (!(sub_dir = vmfs_file_open_rec(vol,rec)))
         return(-1);

#if 0 /* TODO */
      if (cur_dir != vol->root_dir)
         vmfs_file_close(cur_dir);
#endif
      cur_dir = sub_dir;
      ptr = sl + 1;
   }
}

/* Resolve pointer blocks */
static int vmfs_file_resolve_pb(vmfs_file_t *f,m_u32_t blk_id)
{
   u_char buf[4096];
   vmfs_bitmap_header_t *pbc_bmh;
   vmfs_file_t *pbc;
   m_u32_t pbc_blk,dblk;
   m_u32_t subgroup,number;
   size_t len;
   ssize_t res;
   off_t addr;
   int i;

   pbc = f->vol->pbc;
   pbc_bmh = &f->vol->pbc_bmh;

   subgroup = VMFS_BLK_PB_SUBGROUP(blk_id);
   number   = VMFS_BLK_PB_NUMBER(blk_id);

   /* Compute the address of the indirect pointers block in the PBC file */
   pbc_blk = (number * pbc_bmh->items_per_bitmap_entry) + subgroup;
   addr = vmfs_bitmap_get_block_addr(pbc_bmh,pbc_blk);
   len  = pbc_bmh->data_size;

   vmfs_file_seek(pbc,addr,SEEK_SET);

   while(len > 0) {
      res = vmfs_file_read(pbc,buf,sizeof(buf));

      if (res != sizeof(buf))
         return(-1);

      for(i=0;i<res/4;i++) {
         dblk = read_le32(buf,i*4);
         vmfs_blk_list_add_block(&f->blk_list,dblk);
      }

      len -= res;
   }
   
   return(0);
}

/* Bind meta-file info */
static int vmfs_file_bind_meta_info(vmfs_file_t *f,u_char *fmi_buf)
{
   m_u32_t blk_id,blk_type;
   int i;

   vmfs_fmi_read(&f->file_info,fmi_buf);
   vmfs_blk_list_init(&f->blk_list);

   for(i=0;i<VMFS_FILEINFO_BLK_COUNT;i++) {
      blk_id   = read_le32(fmi_buf,VMFS_FILEINFO_OFS_BLK_ARRAY+(i*4));
      blk_type = VMFS_BLK_TYPE(blk_id);

      if (!blk_id)
         break;

      switch(blk_type) {
         /* Full-Block/Sub-Block: simply add it to the list */
         case VMFS_BLK_TYPE_FB:
         case VMFS_BLK_TYPE_SB:
            vmfs_blk_list_add_block(&f->blk_list,blk_id);
            break;

         /* Pointer-block: resolve links */
         case VMFS_BLK_TYPE_PB:
            if (vmfs_file_resolve_pb(f,blk_id) == -1) {
               fprintf(stderr,"VMFS: unable to resolve blocks\n");
               return(-1);
            }
            break;

         default:
            fprintf(stderr,
                    "vmfs_file_bind_meta_info: "
                    "unexpected block type 0x%2.2x!\n",
                    blk_type);
            return(-1);
      }
   }

   return(0);
}

/* Open a file based on a file record */
static vmfs_file_t *
vmfs_file_open_rec(vmfs_volume_t *vol,vmfs_file_record_t *rec)
{
   u_char buf[VMFS_FILE_INFO_SIZE];
   vmfs_file_t *f;

   if (!(f = vmfs_file_create_struct(vol)))
      return NULL;
   
   /* Read the meta-info */
   if (vmfs_get_meta_info(vol,rec,buf) == -1) {
      fprintf(stderr,"VMFS: Unable to get meta-info\n");
      return NULL;
   }

   /* Bind the associated meta-info */
   if (vmfs_file_bind_meta_info(f,buf) == -1)
      return NULL;

   return f;
}

/* Open a file */
vmfs_file_t *vmfs_file_open(vmfs_volume_t *vol,char *filename)
{
   vmfs_file_record_t rec;
   char *tmp_name;
   int res;

   if (!(tmp_name = strdup(filename)))
      return NULL;

   res = vmfs_resolve_path(vol,tmp_name,&rec);
   free(tmp_name);

   if (res != 1)
      return NULL;

   return(vmfs_file_open_rec(vol,&rec));
}

/* Dump a file */
int vmfs_file_dump(vmfs_file_t *f,off_t pos,size_t len,FILE *fd_out)
{
   u_char *buf;
   ssize_t res;
   size_t clen,buf_len;

   if (!len)
      len = vmfs_file_get_size(f);

   buf_len = 0x100000;

   if (!(buf = malloc(buf_len)))
      return(-1);

   vmfs_file_seek(f,pos,SEEK_SET);

   while(len > 0) {
      clen = m_min(len,buf_len);
      res = vmfs_file_read(f,buf,clen);

      if (res < 0) {
         fprintf(stderr,"vmfs_file_dump: problem reading input file.\n");
         return(-1);
      }

      if (fwrite(buf,1,res,fd_out) != res) {
         fprintf(stderr,"vmfs_file_dump: error writing output file.\n");
         return(-1);
      }

      if (res < clen)
         break;

      len -= res;
   }

   free(buf);
   return(0);
}

/* ======================================================================== */
/* Mounted volume management                                                */
/* ======================================================================== */

/* Get block size of a volume */
static inline m_u64_t vmfs_vol_get_blocksize(vmfs_volume_t *vol)
{
   return(vol->fs_info.block_size);
}

/* Read a data block from the physical volume */
ssize_t vmfs_vol_read_data(vmfs_volume_t *vol,off_t pos,u_char *buf,size_t len)
{
   if (fseeko(vol->fd,pos,SEEK_SET) != 0)
      return(-1);

   return(fread(buf,1,len,vol->fd));
}

/* Read a block */
ssize_t vmfs_vol_read(vmfs_volume_t *vol,m_u32_t blk,off_t offset,
                      u_char *buf,size_t len)
{
   off_t pos;

   pos  = (m_u64_t)blk * vmfs_vol_get_blocksize(vol);
   pos += vol->vmfs_base + 0x1000000;
   pos += offset;

   return(vmfs_vol_read_data(vol,pos,buf,len));
}

/* Create a volume structure */
vmfs_volume_t *vmfs_vol_create(char *filename,int debug_level)
{
   vmfs_volume_t *vol;

   if (!(vol = calloc(1,sizeof(*vol))))
      return NULL;

   if (!(vol->filename = strdup(filename)))
      goto err_filename;

   if (!(vol->fd = fopen(vol->filename,"r"))) {
      perror("fopen");
      goto err_open;
   }

   vol->debug_level = debug_level;
   return vol;

 err_open:
   free(vol->filename);
 err_filename:
   free(vol);
   return NULL;
}

/* Read the root directory given its meta-info */
static int vmfs_read_rootdir(vmfs_volume_t *vol,u_char *fmi_buf)
{
   if (!(vol->root_dir = vmfs_file_create_struct(vol)))
      return(-1);

   if (vmfs_file_bind_meta_info(vol->root_dir,fmi_buf) == -1) {
      fprintf(stderr,"VMFS: unable to bind meta info to root directory\n");
      return(-1);
   }
   
   return(0);
}

/* Read bitmap header */
static int vmfs_read_bitmap_header(vmfs_file_t *f,vmfs_bitmap_header_t *bmh)
{
   u_char buf[512];

   vmfs_file_seek(f,0,SEEK_SET);

   if (vmfs_file_read(f,buf,sizeof(buf)) != sizeof(buf))
      return(-1);

   return(vmfs_bmh_read(bmh,buf));
}

/* Open a meta-file */
static vmfs_file_t *vmfs_open_meta_file(vmfs_volume_t *vol,char *name,
                                        vmfs_bitmap_header_t *bmh)
{
   u_char buf[VMFS_FILE_INFO_SIZE];
   vmfs_file_record_t rec;
   vmfs_file_t *f;
   off_t fmi_addr;

   if (!(f = vmfs_file_create_struct(vol)))
      return NULL;

   /* Search the file name in root directory */
   if (vmfs_file_searchdir(vol->root_dir,name,&rec) != 1)
      return NULL;
   
   /* Read the meta-info */
   fmi_addr = vmfs_get_meta_info_offset(vol,rec.block_id);
   fmi_addr += vol->fdc_base;

   if (vmfs_vol_read_data(vol,fmi_addr,buf,sizeof(buf)) != sizeof(buf))
      return NULL;

   /* Bind the associated meta-info */
   if (vmfs_file_bind_meta_info(f,buf) == -1)
      return NULL;

   /* Read the bitmap header */
   if ((bmh != NULL) && (vmfs_read_bitmap_header(f,bmh) == -1))
      return NULL;
   
   return f;
}

/* Open all the VMFS meta files */
static int vmfs_open_all_meta_files(vmfs_volume_t *vol)
{
   vol->fbb = vmfs_open_meta_file(vol,VMFS_FBB_FILENAME,&vol->fbb_bmh);
   vol->fdc = vmfs_open_meta_file(vol,VMFS_FDC_FILENAME,&vol->fdc_bmh);
   vol->pbc = vmfs_open_meta_file(vol,VMFS_PBC_FILENAME,&vol->pbc_bmh);
   vol->sbc = vmfs_open_meta_file(vol,VMFS_SBC_FILENAME,&vol->sbc_bmh);
   vol->vh  = vmfs_open_meta_file(vol,VMFS_VH_FILENAME,NULL);

   return(0);
}

/* Dump volume bitmaps */
int vmfs_vol_dump_bitmaps(vmfs_volume_t *vol)
{
   printf("FBB bitmap:\n");
   vmfs_bmh_show(&vol->fbb_bmh);

   printf("\nFDC bitmap:\n");
   vmfs_bmh_show(&vol->fdc_bmh);

   printf("\nPBC bitmap:\n");
   vmfs_bmh_show(&vol->pbc_bmh);

   printf("\nSBC bitmap:\n");
   vmfs_bmh_show(&vol->sbc_bmh);

   return(0);
}

/* Read FDC base information */
static int vmfs_read_fdc_base(vmfs_volume_t *vol)
{
   u_char buf[VMFS_FILE_INFO_SIZE];
   vmfs_file_info_t fmi;
   off_t fmi_pos;
   m_u64_t len;

   /* Read the header */
   if (vmfs_vol_read_data(vol,vol->fdc_base,buf,sizeof(buf)) < sizeof(buf))
      return(-1);

   vmfs_bmh_read(&vol->fdc_bmh,buf);

   if (vol->debug_level > 0) {
      printf("FDC bitmap:\n");
      vmfs_bmh_show(&vol->fdc_bmh);
   }

   /* Read the File Meta Info */
   fmi_pos = vol->fdc_base + vmfs_bitmap_get_area_data_addr(&vol->fdc_bmh,0);

   if (fseeko(vol->fd,fmi_pos,SEEK_SET) == -1)
      return(-1);
   
   printf("File Meta Info at @0x%llx\n",(m_u64_t)fmi_pos);

   len = vol->fs_info.block_size - (fmi_pos - vol->fdc_base);
   printf("Length: 0x%8.8llx\n",len);

   /* Read the root directory meta-info */
   if (fread(buf,vol->fdc_bmh.data_size,1,vol->fd) != 1)
      return(-1);

   vmfs_fmi_read(&fmi,buf);
   vmfs_read_rootdir(vol,buf);

   /* Read the meta files */
   vmfs_open_all_meta_files(vol);

   /* Dump bitmap info */
   if (vol->debug_level > 0)
      vmfs_vol_dump_bitmaps(vol);

   return(0);
}

/* Open a VMFS volume */
int vmfs_vol_open(vmfs_volume_t *vol)
{
   vol->vmfs_base = VMFS_VOLINFO_BASE;

   /* Read volume information */
   if (vmfs_volinfo_read(&vol->vol_info,vol->fd) == -1) {
      fprintf(stderr,"VMFS: Unable to read volume information\n");
      return(-1);
   }

   if (vol->debug_level > 0)
      vmfs_volinfo_show(&vol->vol_info);

   /* Read FS info */
   if (vmfs_fsinfo_read(&vol->fs_info,vol->fd,vol->vmfs_base) == -1) {
      fprintf(stderr,"VMFS: Unable to read FS information\n");
      return(-1);
   }

   if (vol->debug_level > 0)
      vmfs_fsinfo_show(&vol->fs_info);

   /* Compute position of FDC base */
   vol->fdc_base = vol->vmfs_base + VMFS_FDC_BASE;

   if (vol->debug_level > 0)
      printf("FDC base = @0x%llx\n",(m_u64_t)vol->fdc_base);

   /* Read FDC base information */
   if (vmfs_read_fdc_base(vol) == -1) {
      fprintf(stderr,"VMFS: Unable to read FDC information\n");
      return(-1);
   }

   printf("VMFS: volume opened successfully\n");
   return(0);
}

int main(int argc,char *argv[])
{
   vmfs_volume_t *vol;

   vol = vmfs_vol_create(argv[1],2);
   vmfs_vol_open(vol);

#if 0
   {
      FILE *fd;
      fd = fopen("fdc_dump","w");
      vmfs_file_dump(vol->fdc,0,0,fd);
      fclose(fd);
   }
#endif

#if 0
   {
      vmfs_file_t *f;
      FILE *fd;

      fd = fopen("test.vmx","w");
      f = vmfs_file_open(vol,"Test1/Test1.vmx");
      vmfs_file_dump(f,0,0,fd);
      fclose(fd);
   }
#endif

#if 0
   {
      m_u32_t count;
      
      count = vmfs_bitmap_allocated_items(vol->fbb,&vol->fbb_bmh);
      printf("FBB allocated items: %d (0x%x)\n",count,count);
      vmfs_bitmap_check(vol->fbb,&vol->fbb_bmh);

      count = vmfs_bitmap_allocated_items(vol->fdc,&vol->fdc_bmh);
      printf("FDC allocated items: %d (0x%x)\n",count,count);
      vmfs_bitmap_check(vol->fdc,&vol->fdc_bmh);

      count = vmfs_bitmap_allocated_items(vol->pbc,&vol->pbc_bmh);
      printf("PBC allocated items: %d (0x%x)\n",count,count);
      vmfs_bitmap_check(vol->pbc,&vol->pbc_bmh);

      count = vmfs_bitmap_allocated_items(vol->sbc,&vol->sbc_bmh);
      printf("SBC allocated items: %d (0x%x)\n",count,count);
      vmfs_bitmap_check(vol->sbc,&vol->sbc_bmh);

   }
#endif

#if 1
   vmfs_heartbeat_show_active(vol);
#endif

   return(0);
}
