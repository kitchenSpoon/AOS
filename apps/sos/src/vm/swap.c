#include <strings.h>

#define NUM_BITS (32)

int swap_init(){
    bzero(free_slots, sizeof(free_slots));
    return 0;
}

int swap_find_free_slots(){
    //loops 32 times
    for(uint32_t i = 0; i < NUM_FREE_SLOTS; i++){
        for(uint32_t j = 0; j < NUM_BITS; j++){
            if(!(free_slots[i] & (1<<j))){
                return i*NUM_FREE_SLOTS + j;
            }
        }
    }

    return -1;
}

int swap_in(addr as, seL4_Word addr){
    //use as's mapping between addr and offset
    //int offset = as->mapping[addr];
    //nfs_read();
}

int swap_out(){
    int slot = swap_find_free_slots();
    if(slot == -1){
        return -1;
    }

}
