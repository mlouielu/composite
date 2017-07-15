#include "cFE_util.h"
#include "osfilesys.h"

struct fs filesystem[MAX_NUM_FS];
struct fs *openfs;
struct fsobj *openfile;
struct fsobj files[MAX_NUM_FILES];
struct cos_compinfo *ci;

/******************************************************************************
** Tar Level Methods
******************************************************************************/

/*
 * Loads the position in memory of linked tar file system
 * Checks for badly linked or no linked tar file.  The names given by
 * the linker are non-intuitive so a description of error checking is given:
 * First checks to make sure that symbols have been overwritten by linking
 * process.  Next checks that the size is greater than 0.  Finally checks that
 * the end of the tar is after the start
 */
uint32 tar_load()
{
    if (!_binary_cFEfs_tar_start)
        return OS_FS_ERR_DRIVE_NOT_CREATED;
    if (! &_binary_cFEfs_tar_size)
        return OS_FS_ERR_DRIVE_NOT_CREATED;
    if (&_binary_cFEfs_tar_end < &_binary_cFEfs_tar_start)
        return OS_FS_ERR_DRIVE_NOT_CREATED;
    size    = (size_t) &_binary_cFEfs_tar_size;
    start   = &_binary_cFEfs_tar_start;
    end     = &_binary_cFEfs_tar_end;
    return  OS_FS_SUCCESS;
}

/*
 * parses a loaded tar into filesystem[0]
 * precondition: tar has been loaded by tar_load, a filesystem has been initialized
 * with newfs with the devname of tar
 * Postcondition: A proper error code is returned OR the tar is represented in memory
 * at filesystem 0
 */
uint32 tar_parse()
{
    assert(start && end);
    assert(size < INT32_MAX);
    assert(end - start > 0);
    assert(size == (size_t) (end - start));
    uint32 i;
    openfs = &filesystem[0];
    uint32 offset = 0;
    struct fsobj *o;

    while (offset + start < end) {
        if (newfile_get(&o))            return OS_FS_ERR_DRIVE_NOT_CREATED;

        //tar ends after two empty records
        if ( !(offset + start)[0] && !(offset + start)[TAR_BLOCKSIZE]) {
            o->filedes = 0;
            return OS_FS_SUCCESS;
        }

        if (tar_cphdr(offset, o))           return OS_FS_ERR_DRIVE_NOT_CREATED;
        if (file_insert(o, offset + start)) return OS_FS_ERR_DRIVE_NOT_CREATED;

        /*
         * data is aligned to 512 byte blocks.  a header is 500 bytes, and
         * the file's data begins exactly after the header
         * therefor the next header is 500 + o->size rounded up to a mult of 512
         */
         offset += round_to_blocksize(o->size + 500);
    }

    //tar ends before two empty records are found
    return OS_FS_ERROR;
}

/*
 * Copies information from a tar file header to a fsobj
 */
uint32 tar_cphdr(uint32 tar_offset, struct fsobj *file)
{
    assert(tar_offset < size);
    assert(file->filedes > 0);
    struct f_part *part;
    newpart_get(&part);
    part->memtype = STATIC;
    char *location = start;
    location += tar_offset;
    memmove(location + 1, location, strlen(location));
    location[0] = '/';
    file->name = path_to_name(location);
    if (*(location + strlen(location) - 1) == '/') {
        file->type              = FSOBJ_DIR;
        file->size              = 0;
    }
    else{
        file->type              = FSOBJ_FILE;
        file->size              = oct_to_dec(location + 124);
        file->file_part         = part;
        file->file_part->data   = location + 500;
    }
    return OS_FS_SUCCESS;
}

/******************************************************************************
** fsobj Level Methods
******************************************************************************/

int32 chk_fd(int32 filedes)
{
    if (filedes > OS_MAX_NUM_OPEN_FILES)return OS_FS_ERR_INVALID_FD;
    if (filedes == 0)                   return OS_FS_ERR_INVALID_FD;

    struct fsobj *o = &files[filedes - 1];

    if (o->filedes != filedes)          return OS_FS_ERR_INVALID_FD;
    return OS_FS_SUCCESS;
}

uint32 newpart_get(struct f_part **part)
{
    if (ci == NULL) {
        struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
        ci = &(defci->ci);
    }
    *part = cos_page_bump_alloc(ci);
    assert(part != NULL);
    (*part)->next = NULL;
    (*part)->prev = NULL;
    (*part)->file = NULL;
    (*part)->data = (char *) part + sizeof(part);
    return OS_FS_SUCCESS;
}

//finds the next free file
uint32 newfile_get(struct fsobj **o)
{
    uint32 count = 0;
    while (count < MAX_NUM_FILES && files[count].filedes) {
        count++;
    }
    if (count == MAX_NUM_FILES) return OS_FS_ERROR;
    *o =  &files[count];
    (*o)->filedes = count + 1;
    //filedes needs to be unique and nonzero, so filedes is defined as index+1
    return OS_FS_SUCCESS;
}

uint32 file_insert(struct fsobj *o, char *path)
{
    //paths should always begin with '/' but we do not need it here
    if (path[0] != '/') return OS_FS_ERR_PATH_INVALID;
    path++;

    if (openfs->root == NULL) {
        openfs->root = o;
        return OS_FS_SUCCESS;
    }
    assert(o && path && openfs);
    assert(openfs->root && openfs->root->filedes);
    if (openfs->root == o) return OS_FS_SUCCESS;

    struct fsobj *root = openfs->root;
    assert(root->name);
    //loop terminates when it finds a place to put o, either at the end of a
    //list of children or as the first in an empty children list
    while (1) {
        //if there is no child, then insert as child
        if (!root->child) {
            o->parent = root;
            root->child = o;
            return OS_FS_SUCCESS;
        }
        root = root->child;

        do {
            path++;
        } while (path[0]!= '/');

        //precondition: root is the first in a non-empty list of children
        //postcondition: root is an ancestor of o or o has been inserted in list
        while (strspn(path, root->name) < strlen(root->name)) {
            if (!root->next) {
                root->next = o;
                o->prev = root;
                o->parent = root->parent;
                return OS_FS_SUCCESS;
            }
            root = root->next;
        }

    }
    PANIC("Unreachable Statement");
    return 0;
}

int32 file_open(const char *path) {
    assert(openfs);
    openfs = &(filesystem[0]);
    assert(openfs->root);
    struct fsobj *file = file_find(path);
    if (file == NULL) return OS_FS_ERROR;
    assert(1 > file->refcnt); // we do not yet support concurrent file access
    file->refcnt++;
    file->position.open_part    = file->file_part;
    file->position.addr         = file->position.open_part->data;
    file->position.file_offset  = 0;
    file->position.part_offset  = 0;
    return file->filedes;
}

int32 file_close(int32 filedes) {

    int32 ret = chk_fd(filedes);
    if (ret) return ret;

    uint32 index = filedes + 1;
    if (files[index].refcnt < 1)                    return OS_FS_ERROR;
    files[index].refcnt--;
    return OS_FS_SUCCESS;
}

int32 file_read(int32  filedes, void *buffer, uint32 nbytes)
{
    if (!buffer)                                    return OS_FS_ERR_INVALID_POINTER;
    int32 ret = chk_fd(filedes);
    if (ret) return ret;

    struct fsobj *o = &files[filedes - 1];
    struct f_part *part = o->position.open_part;

    if (o->refcnt < 1)                              return OS_FS_ERROR;
    if (o->type == FSOBJ_DIR)                       return OS_FS_ERROR;

    // nbytes > number of bytes left in file, only number left are read
    if (nbytes > o->size - o->position.file_offset) {
        nbytes = o->size - o->position.file_offset;
    }

    if (nbytes == 0)            return 0;
    uint32 bytes_to_read        = nbytes;

    if (o->file_part->memtype == DYNAMIC){
        uint32 read_size; //the length of a continuous segment to be read from
        while (1) {
            read_size = F_PART_DATA_SIZE - o->position.file_offset;
            part = o->position.open_part;

            if (bytes_to_read > read_size) {

                memcpy(buffer, &part->data[o->position.part_offset], read_size);

                buffer                      += read_size;
                bytes_to_read               -= read_size;
                o->position.file_offset     += read_size;

                if (part->next == NULL) {
                    o->position.part_offset = F_PART_DATA_SIZE;
                    return nbytes - bytes_to_read;
                }

                o->position.open_part   = part->next;
                o->position.part_offset = 0;
                o->position.addr        = part->data;

            } else if (bytes_to_read == read_size) {

                memcpy(buffer, &part->data[o->position.part_offset], read_size);

                o->position.file_offset     += read_size;
                o->position.addr            += read_size;

                if (part->next == NULL) {
                    o->position.part_offset = F_PART_DATA_SIZE;
                    return nbytes;
                }

                o->position.open_part       = part->next;
                o->position.part_offset     = 0;
                return nbytes;

            // bytes_to_read < the continuous space left on f_part
            } else {
                memcpy(buffer, &part->data[o->position.part_offset], bytes_to_read);
                o->position.part_offset     += bytes_to_read;
                o->position.file_offset     += bytes_to_read;
                o->position.addr            += bytes_to_read;
                return OS_FS_SUCCESS;
            }
        }
    } else {
        memcpy(buffer, &part->data[o->position.file_offset], bytes_to_read);
        o->position.part_offset             += bytes_to_read;
        o->position.file_offset             += bytes_to_read;
        o->position.addr                    += bytes_to_read;
        return nbytes;
    }

    PANIC("Unreachable Statement");
    return 0;
}

int32 file_write(int32 filedes, void *buffer, uint32 nbytes)
{
    if (!buffer)                        return OS_FS_ERR_INVALID_POINTER;
    int32 ret = chk_fd(filedes);
    if (ret) return ret;

    struct fsobj *o = &files[filedes - 1];

    if (o->refcnt < 1)                  return OS_FS_ERROR;
    if (o->file_part->memtype == STATIC)return OS_FS_ERROR;
    if (o->type == FSOBJ_DIR)           return OS_FS_ERROR;

    if (nbytes == 0)                    return 0;
    uint32 bytes_to_write = nbytes;
    uint32 bytes_remaining = F_PART_DATA_SIZE - o->position.part_offset;

    //while there are enough bytes to be written to fill a f_part
    while (bytes_to_write > bytes_remaining) {
        memcpy(o->position.addr, buffer, bytes_remaining);
        o->position.file_offset         += bytes_remaining;
        buffer                          += bytes_remaining;
        bytes_to_write                  -= bytes_remaining;
        o->position.part_offset         = 0;
        if (o->position.open_part->next == NULL) {
            struct f_part *part;
            newpart_get(&part);
            part->memtype               = DYNAMIC;
            part->file                  = o;
            part->next                  = NULL;
            part->prev                  = o->position.open_part;
            o->position.open_part->next = part;
        }

        o->position.open_part           = o->position.open_part->next;
        o->position.addr                = o->position.open_part->data;
        bytes_remaining                 = F_PART_DATA_SIZE - o->position.part_offset;
    }

    //bytes_to_write < bytes_remaining
    memcpy(o->position.addr, buffer, bytes_to_write);
    o->position.addr                    += bytes_to_write;
    o->position.part_offset             += bytes_to_write;
    o->position.file_offset             += bytes_to_write;
    if (o->size < o->position.file_offset) {
        o->size                         = o->position.file_offset;
    }
    return nbytes;
}

void file_rewinddir()
{
    if (!openfile) return;
    if (openfile->parent == NULL) return;
    assert(openfile->parent->child);
    openfile = openfile->parent->child;
    return;
}

struct fsobj *file_find(const char *path)
{
    //paths should always begin with '/' dir names do not
    if (path[0] != '/') return NULL;
    path++;

    assert(path);
    if(!openfs || !openfs->root) return NULL;
    struct fsobj *root = openfs->root;
    assert(root && root->name);
    if (strlen(root->name) > strlen(path)) return NULL;

    while (1) {

        // iterate through linked list of children
        while (strspn(path, root->name) < strlen(root->name)) {
            if (!strcmp(root->name, path)) return root;
            if (!root->next) return NULL;
            root = root->next;
        }

        //root must now be parent of final location
        if (!strcmp(root->name, path)) return root;
        assert(strspn(path, root->name) >= strlen(root->name));
        if (!root->child) return NULL;

        root = root->child;
        while (path[0] != '/') {
            path++;
        }
        path++;

    }
    PANIC("Unreachable Statement");
    return NULL;
}

int32 file_mkdir(const char *path, uint32 access)
{
    assert(path);
    struct fsobj *o;
    if (newfile_get(&o)) return OS_FS_ERR_DRIVE_NOT_CREATED;
    o->name = path_to_name((char *) path);
    o->type = FSOBJ_DIR;
    o->size = 0;
    if (file_insert(o, (char *) path)) return OS_FS_ERR_PATH_INVALID;

    return OS_FS_SUCCESS;
}

int32 file_rmdir(const char *path)
{
    assert(path);
    struct fsobj *root = file_find(path);
    if (root == NULL) return OS_FS_ERROR;
    struct fsobj *cur;

    while (root->child) {

        // if cur is the last leaf in a list
        if (cur->next == NULL && cur->child == NULL) {
            if (cur->prev != NULL) {
                assert(cur->prev->next == cur);
                cur = cur->prev;
                rm(cur->next);
            }
            else {
                assert(cur->parent->child == cur);
                cur = cur->parent;
                rm(cur->child);
            }
        } else if (cur->child != NULL) {
            cur = cur->child;
        } else { //cur->next !=NULL
            cur = cur->next;
        }
    }

    rm(root);
    return OS_FS_SUCCESS;
}

int32 file_creat(const char *path)
{
    assert(path);
    struct fsobj *o;
    if (newfile_get(&o)) return OS_FS_ERR_DRIVE_NOT_CREATED;
    o->name = (char *) path;
    o->type = FSOBJ_FILE;
    o->size = 0;
    struct f_part *part;
    newpart_get(&part);
    part->memtype = DYNAMIC;
    o->file_part = part;
    part->file = o;
    part->data = (char *) part + sizeof(part);
    if (file_insert(o, (char *) path)) return OS_FS_ERR_DRIVE_NOT_CREATED;

    return OS_FS_SUCCESS;
}

int32 file_remove(const char *path)
{
    struct fsobj *file = file_find(path);
    if (!file) return OS_FS_ERR_PATH_INVALID;
    rm(file);
    return OS_FS_SUCCESS;
}

int32 file_rename(const char *old_filename, const char *new_filename)
{
    struct fsobj *file = file_find(old_filename);
    if (!file) return OS_FS_ERR_PATH_INVALID;
    file->name = path_to_name((char *)new_filename);
    return OS_FS_SUCCESS;
}

// This is not a full implementation of all that is required by posix
// but it is enough to pass UT and is all the info relevant to this FS
int32 file_stat(const char *path, os_fstat_t  *filestats)
{
    struct fsobj *file = file_find(path);
    if (file == NULL) return OS_FS_ERROR;
    filestats->st_dev = 0;
    filestats->st_ino = file->filedes;
    if (file->type == FSOBJ_FILE) {
        filestats->st_mode = S_IFREG;
    }
    if (file->type == FSOBJ_DIR) {
        filestats->st_mode = S_IFDIR;
    }
    filestats->st_size = file->size;
    filestats->st_blksize = F_PART_DATA_SIZE;
    filestats->st_blocks = 1 + file->size % F_PART_DATA_SIZE;
    return OS_FS_SUCCESS;
}

int32 file_lseek(int32  filedes, int32 offset, uint32 whence)
{
    int32 ret = chk_fd(filedes);
    if (ret) return ret;

    struct fsobj *o = &files[filedes - 1];
    uint32 target_offset = 0;

    // wasnt sure if it should be legal to pass negative offset, went with yes
    if (whence == SEEK_SET) {

        if (offset < 0) return OS_FS_ERROR;
        target_offset = offset;

    } else if (whence == SEEK_CUR) {

        if (offset + (int32) o->position.file_offset < 0) return OS_FS_ERROR;
        target_offset += o->position.file_offset;

    } else if (whence == SEEK_END) {

        if (offset + (int32) o->position.file_offset < 0) return OS_FS_ERROR;
        target_offset += o->size;

    } else {
        return OS_FS_ERROR;
    }
    // you cannot write past the end of a static file
    if ( target_offset > o->size && o->file_part->memtype == STATIC) {
        return OS_FS_ERROR;
    }

    o->position.open_part = o->file_part;
    o->position.file_offset = 0;

    while ( target_offset - o->position.file_offset > F_PART_DATA_SIZE) {

        // seeking past the end of a file writes zeros until that position
        if (o->position.open_part->next == NULL) {
            struct f_part *part;
            newpart_get(&part);
            part->memtype               = DYNAMIC;
            part->file                  = o;
            part->next                  = NULL;
            part->prev                  = o->position.open_part;
            o->position.open_part->next = part;
        }

        o->position.open_part = o->position.open_part->next;
        o->position.file_offset += F_PART_DATA_SIZE;

    }

    o->position.file_offset += target_offset % F_PART_DATA_SIZE;
    o->position.part_offset = target_offset % F_PART_DATA_SIZE;
    o->position.addr = o->position.open_part->data + target_offset % F_PART_DATA_SIZE;

    if (o->position.file_offset > size) {
        size = o->position.file_offset;
    }

    assert(o->position.file_offset == target_offset);
    return target_offset;
}

int32 file_cp(const char *src, const char *dest)
{
    struct fsobj *file_src, *file_dest;
    int32 src_desc = file_open(src);
    if (src_desc <= 0 || src_desc > MAX_NUM_FILES) return OS_FS_ERROR;
    file_src = &files[src_desc - 1];

    newfile_get(&file_dest);

    if(file_src == NULL || file_src->type == FSOBJ_DIR){
        return OS_FS_ERR_INVALID_POINTER;
    }

    file_insert(file_dest, (char *) dest);
    file_dest->name = path_to_name( (char *) dest);
    file_dest->size = 0;
    file_dest->mode = file_src->mode;
    file_dest->type = FSOBJ_FILE;

    /*
     * yeah, this is sub-optimal
     * but I don't expect this to be called at all, much less with large files
     */

    uint32 dest_desc = file_dest->filedes;

    char temp_buff[file_src->size];
    file_read(src_desc, temp_buff,file_src->size);
    file_write(file_dest->filedes, temp_buff, file_src->size);

    return OS_FS_SUCCESS;
}

int32 file_mv(const char *src, const char *dest)
{

    struct fsobj *file;
    uint32 desc = file_open(src);
    if (desc <= 0 || desc > MAX_NUM_FILES) return OS_FS_ERROR;
    file = &files[desc - 1];

    if (file->next) {
        file->next->prev = file->prev;
    }
    if (file->prev) {
        file->prev->next = file->next;
    }
    if (file->parent->child == file) {
        file->parent->child = file->next;
    }

    file_insert(file, (char *) dest);
    file->name = path_to_name((char *) dest);
    return OS_FS_SUCCESS;
}

/*
 * currently conflates name and path.
 * perhaps it would be a good idea to track path in fsobj
 */
int32 file_FDGetInfo(int32 filedes, OS_FDTableEntry *fd_prop)
{
    struct fsobj *o = &files[filedes - 1];

    if (o->filedes != filedes) return OS_FS_ERR_INVALID_FD;
    if (o->refcnt > 0) return OS_FS_ERROR;
    fd_prop->OSfd = o->filedes;
    memcpy(&fd_prop->Path,o->name, strlen(o->name));
    fd_prop->User = 0;
    fd_prop->IsValid = 1;
    return OS_FS_SUCCESS;
}


struct dirent *file_readdir(int32 filedes)
{
    if ()
    struct dirent *p = NULL;i
    return p;
}

/******************************************************************************
** fs Level Methods
******************************************************************************/

uint32 fs_mount(const char *devname, char *mountpoint)
{
    uint32 i;
    assert(devname);
    for (i=0 ; i < MAX_NUM_FS && filesystem[i].devname != NULL ; i++) {
        if (!strcmp(filesystem[i].devname, devname)) {

            struct fsobj *o;
            if (newfile_get(&o))        return OS_FS_ERROR;
            filesystem[i].mountpoint    = mountpoint;
            openfs                      = &filesystem[i];
            if (filesystem[i].root == NULL) {
                file_mkdir(mountpoint, 0);
            }
            return OS_FS_SUCCESS;
        }
    }
    return OS_FS_ERROR;
}

uint32 fs_unmount(const char *mountpoint) {
    uint32 i;
    assert(mountpoint);
    for (i = 0 ; i < MAX_NUM_FS && filesystem[i].mountpoint != NULL ; i++) {
        if (mountpoint != NULL && !strcmp(filesystem[i].mountpoint, mountpoint)) {
            filesystem[i].mountpoint = NULL;
            return OS_FS_SUCCESS;
        }
    }
    return OS_FS_ERROR;
}

/*
 * Given a pointer to an FS, inits values and fsobj for root
 */
uint32 fs_init(struct fs *filesys, char *devname, char *volname, uint32 blocksize, uint32 numblocks)
{
    filesys->devname        = devname;
    filesys->volname        = volname;
    filesys->mountpoint     = "";
    filesys->blocksize      = blocksize;
    filesys->numblocks      = numblocks;
    filesys->root           = NULL;
    return OS_FS_SUCCESS;
}

uint32 newfs_init(char *devname, char *volname, uint32 blocksize, uint32 numblocks)
{
    uint32 count = 1, ret = 0;
    if (!devname)                           return OS_FS_ERR_INVALID_POINTER;
    if (blocksize == 0 || numblocks == 0)   return OS_FS_ERROR;
    if (strlen(devname) >= OS_FS_DEV_NAME_LEN || strlen(volname) >= OS_FS_VOL_NAME_LEN)
        return OS_FS_ERROR;


    // the first filesystem is always the tar
    if (filesystem[0].devname == NULL) {
        ret = tar_load();
        if (ret != OS_FS_SUCCESS) return ret;
        ret = fs_init(&filesystem[0], devname, volname, blocksize, numblocks);
        if (ret != OS_FS_SUCCESS) return ret;
        return OS_FS_SUCCESS;
    }

    // filesystem[0] is initialized during OS_FS_Init
    if (strcmp(devname, filesystem[0].devname) == 0) return OS_SUCCESS;

    while (count < MAX_NUM_FS && filesystem[count].devname) {
        count++;
    }
    if (count == MAX_NUM_FS)
        return OS_FS_ERR_DRIVE_NOT_CREATED;

    ret = fs_init(&filesystem[count], devname, volname, blocksize, numblocks);
    if (ret !=OS_FS_SUCCESS) return ret;
    openfs = &filesystem[count];
    return OS_FS_SUCCESS;
}

int32 rmfs(char *devname)
{
    if (!devname) return OS_FS_ERR_INVALID_POINTER;

    uint32 i;
    for (i = 0 ; i < MAX_NUM_FS && filesystem[i].devname != NULL ; i++) {
        if (devname && filesystem[i].devname && !strcmp(filesystem[i].devname,devname)) {
            filesystem[i].devname = NULL;
            filesystem[i].volname = NULL;
            filesystem[i].mountpoint = NULL;
            filesystem[i].blocksize = 0;
            filesystem[i].numblocks = 0;
            filesystem[i].root = NULL;
            return OS_FS_SUCCESS;
        }
    }
    return OS_FS_ERROR;

}

int32 Filesys_GetPhysDriveName(char * PhysDriveName, char * MountPoint)
{
    uint32 i;
    for (i = 0 ; i < MAX_NUM_FS && filesystem[i].devname != NULL ; i++) {
        if (filesystem[i].mountpoint && !strcmp(filesystem[i].mountpoint,MountPoint)) {
            memcpy(PhysDriveName, "RAM FS\n",7);
            return OS_FS_SUCCESS;
        }
    }
    return OS_FS_ERROR;
}

int32 Filesys_GetFsInfo(os_fsinfo_t *filesys_info)
{
    filesys_info->MaxFds = MAX_NUM_FILES;
    uint32 i, count = 0;
    for (i = 0 ; i < MAX_NUM_FILES ; i++) {
        if (files[i].filedes == 0) count++;
    }
    filesys_info->FreeFds = count;
    filesys_info->MaxVolumes = MAX_NUM_FS;
    for (i = 0, count = 0 ; i < MAX_NUM_FS ; i++){
        if (filesystem[i].devname == NULL) count++;
    }
    filesys_info->FreeVolumes = count;
    return OS_FS_SUCCESS;
}
