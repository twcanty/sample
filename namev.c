#include "kernel.h"
#include "globals.h"
#include "types.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "fs/dirent.h"
#include "fs/fcntl.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"



/* This takes a base 'dir', a 'name', its 'len', and a result vnode.
 * Most of the work should be done by the vnode's implementation
 * specific lookup() function.
 *
 * If dir has no lookup(), return -ENOTDIR.
 *
 * Note: returns with the vnode refcount on *result incremented.
 */
int
lookup(vnode_t *dir, const char *name, size_t len, vnode_t **result)
{
    dbg(DBG_VFS, "ENTER\n");
        KASSERT(dir);
        KASSERT(name);
        dbg(DBG_CORE, "1\n");
        if (!S_ISDIR(dir->vn_mode)) {
            dbg(DBG_VFS, "ERROR -ENOTDIR\n");
            return -ENOTDIR;
        }
        if (len > NAME_LEN) {
            dbg(DBG_VFS, "ERROR -ENAMETOOLONG\n");
            return -ENAMETOOLONG;
        }
        if (!strcmp(name, ".") || len == 0) {
            vref(dir);
            *result = dir;
            return 0;
        }
        //dbg(DBG_VFS, "2\n");
        KASSERT(dir->vn_ops);
        KASSERT(dir->vn_ops->lookup);
        if (dir->vn_ops->lookup) {
            //dbg(DBG_VFS, "3\n");
            //dbg(DBG_VFS, "dir: %p, name: %s, len: %d, result: %p", dir, name, len, result);
            int ret = dir->vn_ops->lookup(dir, name, len, result);

            //dbg(DBG_VFS, "4\n");
            if (ret < 0 ){
                return ret;
            }
            vnode_t *vn = *result;

        }
        else return -ENOTDIR;

        dbg(DBG_VFS, "EXIT RET 0\n");
        return 0;
}


/* When successful this function returns data in the following "out"-arguments:
 *  o res_vnode: the vnode of the parent directory of "name"
 *  o name: the `basename' (the element of the pathname)
 *  o namelen: the length of the basename
 *
 * For example: dir_namev("/s5fs/bin/ls", &namelen, &name, NULL,
 * &res_vnode) would put 2 in namelen, "ls" in name, and a pointer to the
 * vnode corresponding to "/s5fs/bin" in res_vnode.
 *
 * The "base" argument defines where we start resolving the path from:
 * A base value of NULL means to use the process's current working directory,
 * curproc->p_cwd.  If pathname[0] == '/', ignore base and start with
 * vfs_root_vn.  dir_namev() should call lookup() to take care of resolving each
 * piece of the pathname.
 *
 * Note: A successful call to this causes vnode refcount on *res_vnode to
 * be incremented.
 */
int
dir_namev(const char *pathname, size_t *namelen, const char **name,
          vnode_t *base, vnode_t **res_vnode)
{
        dbg(DBG_VFS, "ENTER\n");
        if (strlen(pathname) > MAXPATHLEN) return -ENAMETOOLONG;
        if (!strlen(pathname)) return -EINVAL;

        //find base using pathname or arguments
        vnode_t *dir;
        if (pathname[0] == '/') {
            dir = vfs_root_vn;
            pathname++;//move past first '/'
        }
        else if (!base) {
            dir = curproc->p_cwd;
        }
        else {
            dir = base;
        }
        vref(dir);
        char *curr_seg = (char *) pathname;
        char *next_slash = (char *) pathname;
        char *end = (char *) pathname + strlen(pathname);
        vnode_t *result;

        //loop through all segments of the pathname
        while (curr_seg != end) {
            if (!dir) return -ENOENT;
            else if (!S_ISDIR(dir->vn_mode)) {
                vput(dir);
                return -ENOTDIR;
            }
            vnode_t *vn = dir;

            //strchrr gives start of next slash
            if ((next_slash = strchr(curr_seg, '/'))) {
                int seg_length = next_slash - curr_seg;
                if (seg_length > NAME_LEN) {
                    vput(dir);
                    return -ENAMETOOLONG;
                }

                //lookup next seg
                int ret = lookup(dir, curr_seg, seg_length, &result);
                if (ret < 0) {
                    vput(dir);
                    return ret;
                }

                //move on
                vput(dir);
                dir = result;
                next_slash++;
                curr_seg = next_slash;
            }
            else break;
        }
        *name = curr_seg;
        *namelen = end - curr_seg;
        *res_vnode = dir;

        dbg(DBG_VFS, "EXIT\n");
        return 0;

}
 

/* This returns in res_vnode the vnode requested by the other parameters.
 * It makes use of dir_namev and lookup to find the specified vnode (if it
 * exists).  flag is right out of the parameters to open(2); see
 * <weenix/fnctl.h>.  If the O_CREAT flag is specified, and the file does
 * not exist call create() in the parent directory vnode.
 *
 * Note: Increments vnode refcount on *res_vnode.
 */
int
open_namev(const char *pathname, int flag, vnode_t **res_vnode, vnode_t *base)
{
        dbg(DBG_VFS, "ENTER\n");
        KASSERT(pathname);
        vnode_t *result;
        size_t namelen = 0;
        const char *name = NULL;
        int ret = dir_namev(pathname, &namelen, &name, base, &result);
        if (ret < 0) {
            return ret;
        }

        if (!S_ISDIR(result->vn_mode)) {
            vput(result);
            dbg(DBG_VFS, "ERROR -ENOTDIR\n");
            return -ENOTDIR;
        }

        //MUTEX ADDED as global variable in kmain.c
        kmutex_lock(vfsm);
        ret = lookup(result, name, namelen, res_vnode);
        if (ret < 0) {
            //create file if it needs to be created
            if (flag&O_CREAT) {
                int i = result->vn_ops->create(result, name, namelen, res_vnode);
                if (i < 0) {
                    vput(result);
                    kmutex_unlock(vfsm);
                    return i;
                }
            }
            else {
                vput(result);
                kmutex_unlock(vfsm);
                return ret;
            }
        }
        
        vput(result);
        kmutex_unlock(vfsm);
        dbg(DBG_VFS, "EXIT 0\n");
        return 0;
}

#ifdef __GETCWD__
/* Finds the name of 'entry' in the directory 'dir'. The name is writen
 * to the given buffer. On success 0 is returned. If 'dir' does not
 * contain 'entry' then -ENOENT is returned. If the given buffer cannot
 * hold the result then it is filled with as many characters as possible
 * and a null terminator, -ERANGE is returned.
 *
 * Files can be uniquely identified within a file system by their
 * inode numbers. */
int
lookup_name(vnode_t *dir, vnode_t *entry, char *buf, size_t size)
{
        NOT_YET_IMPLEMENTED("GETCWD: lookup_name");
        return -ENOENT;
}


/* Used to find the absolute path of the directory 'dir'. Since
 * directories cannot have more than one link there is always
 * a unique solution. The path is writen to the given buffer.
 * On success 0 is returned. On error this function returns a
 * negative error code. See the man page for getcwd(3) for
 * possible errors. Even if an error code is returned the buffer
 * will be filled with a valid string which has some partial
 * information about the wanted path. */
ssize_t
lookup_dirpath(vnode_t *dir, char *buf, size_t osize)
{
        NOT_YET_IMPLEMENTED("GETCWD: lookup_dirpath");

        return -ENOENT;
}
#endif /* __GETCWD__ */