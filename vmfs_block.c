/* 
 * VMFS blocks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <uuid/uuid.h>

#include "utils.h"
#include "vmfs.h"

/* Initialize an empty block list */
void vmfs_blk_list_init(vmfs_blk_list_t *list)
{
   list->avail = list->total = 0;
   list->head = list->tail = NULL;
}

/* Add a new block at tail of a block list */
int vmfs_blk_list_add_block(vmfs_blk_list_t *list,m_u32_t blk_id)
{
   vmfs_blk_array_t *array;
   int pos;

   if (list->avail == 0) {
      /* no room available, create a new array */
      if (!(array = malloc(sizeof(*array))))
         return(-1);

      if (list->tail != NULL)
         list->tail->next = array;
      else
         list->head = array;
      
      list->tail = array;
      
      list->avail = VMFS_BLK_ARRAY_COUNT;
   }

   pos = VMFS_BLK_ARRAY_COUNT - list->avail;

   list->tail->blk[pos] = blk_id;
   list->total++;
   list->avail--;
   return(0);
}

/* Get a block ID from a block list, given its position */
int vmfs_blk_list_get_block(vmfs_blk_list_t *list,u_int pos,m_u32_t *blk_id)
{
   vmfs_blk_array_t *array;
   u_int cpos = 0;

   if (pos >= list->total)
      return(-1);

   for(array=list->head;array;array=array->next) {
      if ((pos >= cpos) && (pos < (cpos + VMFS_BLK_ARRAY_COUNT))) {
         *blk_id = array->blk[pos - cpos];
         return(0);
      }
      
      cpos += VMFS_BLK_ARRAY_COUNT;
   }

   return(-1);
}
