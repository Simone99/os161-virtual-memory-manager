#include "swapfile.h"
#include "vm_tlb.h"
#include <uio.h>
#include <kern/stat.h>
#include <vm.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <spl.h>
#include <proc.h>

// S = Swapped bit
//<----------------20------------>|<----6-----><-----6--->|
//_________________________________________________________
//|       Virtual Page Number     |         |S|     PID   |  
//|_______________________________|_______________________|

#define IS_SWAPPED(x) ((x) & 0x00000040)
#define SET_SWAPPED(x, value) (((x) &~ 0x00000040) | (value << 6))
#define SET_PN(entry, pn) ((entry & 0x00000FFF) | (pn << 12))
#define GET_PN(entry) ((entry &~ 0x00000FFF) >> 12)
#define SET_PID(entry, pid) ((entry &~ 0x0000003F) | pid)
#define GET_PID(entry) (entry & 0x0000003F)

struct swapTable{
    struct vnode *fp;
    uint32_t *entries;
    uint32_t size;
};

swap_table swapTableInit(char swap_file_name[]){
    struct stat file_stat;
    swap_table result = kmalloc(sizeof(*result));
    int tmp = vfs_open(swap_file_name, O_RDWR, 0, &result->fp);
    if(tmp)
        panic("VM: Failed to create Swap area\n");
    VOP_STAT(result->fp, &file_stat);
    result->size = file_stat.st_size / PAGE_SIZE;
    result->entries = (uint32_t*)kmalloc(result->size * sizeof(*(result->entries)));
    for(uint32_t i = 0; i < result->size; i++){
        result->entries[i] = SET_SWAPPED(result->entries[i], 1);
    }
    return result;
}

void swapout(swap_table st, uint32_t index, paddr_t paddr, page_table pt){
    int spl=splhigh();
    struct uio swap_uio;
    struct iovec iov;
    uint32_t index_pt, page_number, PID;
    uio_kinit(&iov, &swap_uio, (void*)PADDR_TO_KVADDR(paddr & PAGE_FRAME), PAGE_SIZE, index*PAGE_SIZE, UIO_WRITE);
    index_pt = (paddr & PAGE_FRAME) >> 12;
    page_number = getPageN(pt, index_pt);
    PID = getPID(pt, index_pt);

    // Set page as invalid in IPT
    setInvalid(pt, index_pt);

    // Add page into swap table
    st->entries[index] = SET_PN(SET_PID(SET_SWAPPED(st->entries[index], 0), PID), page_number);

    splx(spl);
    int result = VOP_WRITE(st->fp, &swap_uio);
    if(result)     
        panic("VM_SWAP_OUT: Failed");
    spl=splhigh();
    TLB_Invalidate(paddr);
    splx(spl);
}

void swapin(swap_table st, uint32_t index, paddr_t paddr, page_table pt/*, vaddr_t faultaddress*/){
    int spl=splhigh(), result;
    struct uio swap_uio;
    struct iovec iov;
    uio_kinit(&iov, &swap_uio, (void*)PADDR_TO_KVADDR(paddr & PAGE_FRAME), PAGE_SIZE, index*PAGE_SIZE, UIO_READ);

    // Insert page into page table
    addEntry(pt, GET_PN(st->entries[index]), paddr >> 12, GET_PID(st->entries[index]));

    // Remove page from swap table
    st->entries[index] = SET_SWAPPED(st->entries[index], 1);

    splx(spl);
    result=VOP_READ(st->fp, &swap_uio);

	/*iov.iov_ubase = (userptr_t)(faultaddress & PAGE_FRAME);
	iov.iov_len = PAGE_SIZE;		 // length of the memory space
	swap_uio.uio_iov = &iov;
	swap_uio.uio_iovcnt = 1;
	swap_uio.uio_resid = PAGE_SIZE;          // amount to read from the file
	swap_uio.uio_offset = index*PAGE_SIZE;
	swap_uio.uio_segflg = UIO_USERISPACE;
	swap_uio.uio_rw = UIO_READ;
	swap_uio.uio_space = proc_getas();
    result = VOP_READ(st->fp, &swap_uio);*/
    if(result) 
        panic("VM: SWAPIN Failed");
}

int getFirstFreeChunckIndex(swap_table st){
    for(uint32_t i = 0; i < st->size; i++){
        if(IS_SWAPPED(st->entries[i]))
            return i;
    }
    return -1;
}

void elf_to_swap(swap_table st, struct vnode *v, off_t offset, uint32_t init_page_n, size_t memsize, uint32_t PID){
    int spl=splhigh();
    struct iovec iov_swap, iov_elf;
	struct uio ku_swap, ku_elf;
    char buffer[PAGE_SIZE / 2];
    int chunk_index, result;
    uint32_t n_chuncks = (memsize + PAGE_SIZE - 1) / PAGE_SIZE, i, j, incr = PAGE_SIZE / 2;
    splx(spl);
    for(i = 0; i < n_chuncks - 1; i++, init_page_n++){
        // Get first chunck available
        chunk_index = getFirstFreeChunckIndex(st);
        if(chunk_index == -1){
            // Handle full swapfile
            panic("Swap area full!\n");
            return;
        }else{
            for(j = 0; j < 2; j++, offset += incr){
                // Read one page from elf file
                uio_kinit(&iov_elf, &ku_elf, buffer, incr, offset, UIO_READ);
                result = VOP_READ(v, &ku_elf);
                if(result) 
                    panic("Failed loading elf into swap area!\n");

                // Write page into swapfile
                uio_kinit(&iov_swap, &ku_swap, buffer, incr, chunk_index*PAGE_SIZE, UIO_WRITE);
                result = VOP_WRITE(st->fp, &ku_swap);
                if(result) 
                    panic("Failed loading elf into swap area!\n");
                
            }
            // Add page into swap table
            st->entries[chunk_index] = SET_SWAPPED(SET_PID(SET_PN(st->entries[chunk_index], init_page_n), PID), 0);
        }
    }
    chunk_index = getFirstFreeChunckIndex(st);
    if(chunk_index == -1){
        // Handle full swapfile
        panic("Swap area full!\n");
        return;
    }else{
        if(memsize - offset > incr){
            // Read one page from elf file
            uio_kinit(&iov_elf, &ku_elf, buffer, incr, offset, UIO_READ);
            result = VOP_READ(v, &ku_elf);
            if(result) 
                panic("Failed loading elf into swap area!\n");

            // Write page into swapfile
            uio_kinit(&iov_swap, &ku_swap, buffer, incr, chunk_index*PAGE_SIZE, UIO_WRITE);
            result = VOP_WRITE(st->fp, &ku_swap);
            if(result) 
                panic("Failed loading elf into swap area!\n");
            offset += incr;
        }
        uio_kinit(&iov_elf, &ku_elf, buffer, memsize - offset, offset, UIO_READ);
        result = VOP_READ(v, &ku_elf);
        if(result) 
            panic("Failed loading elf into swap area!\n");
        
        // Write page into swapfile
        uio_kinit(&iov_swap, &ku_swap, buffer, incr, chunk_index*PAGE_SIZE, UIO_WRITE);
        result = VOP_WRITE(st->fp, &ku_swap);
        if(result) 
            panic("Failed loading elf into swap area!\n");
        // Add page into swap table
        st->entries[chunk_index] = SET_SWAPPED(SET_PID(SET_PN(st->entries[chunk_index], init_page_n), PID), 0);
    }
    splx(spl);
}

int getSwapChunk(swap_table st, vaddr_t faultaddress){
    uint32_t page_n = faultaddress >> 12;
    for(uint32_t i = 0; i < st->size; i++){
        if(GET_PN(st->entries[i]) == page_n && !IS_SWAPPED(st->entries[i]))
            return i;
    }
    return -1;
}