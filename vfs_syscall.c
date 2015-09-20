/*
 *  FILE: vfs_syscall.c
 *  AUTH: mcc | jal
 *  DESC:
 *  DATE: Wed Apr  8 02:46:19 1998
 *  $Id: vfs_syscall.c,v 1.9.2.2 2006/06/04 01:02:32 afenn Exp $
 */

#include "kernel.h"
#include "errno.h"
#include "globals.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/open.h"
#include "fs/fcntl.h"
#include "fs/lseek.h"
#include "mm/kmalloc.h"
#include "util/string.h"
#include "util/printf.h"
#include "fs/stat.h"
#include "util/debug.h"

/* To read a file:
 *      o fget(fd)
 *      o call its virtual read f_op
 *      o update f_pos
 *      o fput() it
 *      o return the number of bytes read, or an error
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for reading.
 *      o EISDIR
 *        fd refers to a directory.
 *
 * In all cases, be sure you do not leak file refcounts by returning before
 * you fput() a file that you fget()'ed.
 */
int
do_read(int fd, void *buf, size_t nbytes)
{
        dbg(DBG_VFS, "ENTER\n");

        if (!curproc->p_files[fd] || fd<0 || fd>=NFILES) {
                return -EBADF;
        }

        file_t *file = fget(fd);

        if (!(file->f_mode & FMODE_READ)) {
                fput(file);
                return -EBADF;
        }
        if (S_ISDIR(file->f_vnode->vn_mode)) {
                fput(file);
                return -EISDIR;
        }

        int bytes = file->f_vnode->vn_ops->read(file->f_vnode, file->f_pos, buf, nbytes);

        if (bytes < 0) {
                fput(file);
                return bytes;
        }

        file->f_pos = file->f_pos + bytes;
        fput(file);

        dbg(DBG_VFS, "EXIT\n");
        return bytes;
}

/* Very similar to do_read.  Check f_mode to be sure the file is writable.  If
 * f_mode & FMODE_APPEND, do_lseek() to the end of the file, call the write
 * f_op, and fput the file.  As always, be mindful of refcount leaks.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not a valid file descriptor or is not open for writing.
 */
int
do_write(int fd, const void *buf, size_t nbytes)
{
        dbg(DBG_VFS, "ENTER\n");

        if (!curproc->p_files[fd] || fd<0 || fd>NFILES) {
                return -EBADF;
        }

        file_t *file = fget(fd);


        if (!(file->f_mode & FMODE_WRITE)) {
                fput(file);
                return -EBADF;
        }

        //seek end if going to append
        if ((file->f_mode & FMODE_APPEND)) {
                int end = do_lseek(fd, NULL, SEEK_END);
                if (end < 0) {
                        fput(file);
                        return end;
                }
        }

        //otherwise just go from fpos
        KASSERT(file->f_vnode->vn_ops->write);        
        int written = file->f_vnode->vn_ops->write(file->f_vnode, file->f_pos, buf, nbytes);
        if (written < 0) {
                fput(file);
                return written;
        }

        file->f_pos = file->f_pos + written;
        fput(file);

        dbg(DBG_VFS, "EXIT\n");
        return written;
}

/*
 * Zero curproc->p_files[fd], and fput() the file. Return 0 on success
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't a valid open file descriptor.
 */
int
do_close(int fd)
{
        dbg(DBG_VFS, "ENTER\n");

        if (fd<0 || fd>NFILES) {
                return -EBADF;
        }

        file_t *file = fget(fd);

        if (file == NULL || !curproc->p_files[fd]) {
                return -EBADF;
        }

        
        fput(file);
        fput(file);//net 0
        curproc->p_files[fd] = NULL;

        dbg(DBG_VFS, "EXIT\n");
        return 0;
}


/* To dup a file:
 *      o fget(fd) to up fd's refcount
 *      o get_empty_fd()
 *      o point the new fd to the same file_t* as the given fd
 *      o return the new file descriptor
 *
 * Don't fput() the fd unless something goes wrong.  Since we are creating
 * another reference to the file_t*, we want to up the refcount.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd isn't an open file descriptor.
 *      o EMFILE
 *        The process already has the maximum number of file descriptors open
 *        and tried to open a new one.
 */
int
do_dup(int fd)
{
        dbg(DBG_VFS, "ENTER\n");

        if (!curproc->p_files[fd] || fd<0 || fd>NFILES) {
                return -EBADF;
        }
        file_t *file = fget(fd);

        KASSERT(file);

        int dupfd = get_empty_fd(curproc);
        if (!dupfd) {
                fput(file);
                return -EMFILE;
        }

        curproc->p_files[dupfd] = file;
        dbg(DBG_VFS, "EXIT\n");
        return dupfd;
}


/* Same as do_dup, but insted of using get_empty_fd() to get the new fd,
 * they give it to us in 'nfd'.  If nfd is in use (and not the same as ofd)
 * do_close() it first.  Then return the new file descriptor.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        ofd isn't an open file descriptor, or nfd is out of the allowed
 *        range for file descriptors.
 */
int
do_dup2(int ofd, int nfd)
{
        dbg(DBG_VFS, "ENTER\n");
        if (!curproc->p_files[ofd] || ofd<0 || ofd>NFILES) {
                return -EBADF;
        }

        if (nfd < 0 || nfd > NFILES) {
                return -EBADF;
        }

        if (nfd != ofd) {
                if (curproc->p_files[nfd]) {
                        int ret = do_close(nfd);
                        if (ret < 0) {
                                return ret;
                        }
                }

                file_t *file = fget(ofd);

                curproc->p_files[nfd] = file;
                
        }
        dbg(DBG_VFS, "EXIT\n");
        return nfd;
}

/*
 * This routine creates a special file of the type specified by 'mode' at
 * the location specified by 'path'. 'mode' should be one of S_IFCHR or
 * S_IFBLK (you might note that mknod(2) normally allows one to create
 * regular files as well-- for simplicity this is not the case in Weenix).
 * 'devid', as you might expect, is the device identifier of the device
 * that the new special file should represent.
 *
 * You might use a combination of dir_namev, lookup, and the fs-specific
 * mknod (that is, the containing directory's 'mknod' vnode operation).
 * Return the result of the fs-specific mknod, or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        mode requested creation of something other than a device special
 *        file.
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mknod(const char *path, int mode, unsigned devid)
{
        dbg(DBG_VFS, "ENTER\n");
        if (!path) return -EINVAL;

        if (strlen(path) > MAXPATHLEN) {
                return -ENAMETOOLONG;
        }

        if (mode != S_IFCHR && mode != S_IFBLK) {
                return -EINVAL;
        }

        size_t namelen=0;
        const char *name;
        vnode_t *res_vnode;
        int ret = dir_namev(path, &namelen,&name,NULL,&res_vnode);
        if (ret < 0) return ret;

        if (res_vnode == NULL) return -ENOENT;
        if (namelen > NAME_LEN) return -ENAMETOOLONG;

        if (!S_ISDIR(res_vnode->vn_mode)) {
                vput(res_vnode);
                return -ENOTDIR;
        }

        vnode_t *result;
        if (name) {
                KASSERT(res_vnode->vn_ops->lookup);
                int ret2 = lookup(res_vnode, name, namelen, &result);
                if (!ret2) {
                        vput(res_vnode);
                        vput(result);
                        return -EEXIST;
                }
                vnode_t *vn = result;
                // if (vn->vn_vno == 8) {
                // dbg(DBG_KB, "1Vnode %ld mode %x device %x flags %x is still in use with refcount=%d and %d res pages\n",
                //                 (long)vn->vn_vno, vn->vn_mode, vn->vn_devid, vn->vn_flags, vn->vn_refcount, vn->vn_nrespages);
                // }
        }

        vput(res_vnode);
        KASSERT(res_vnode->vn_ops->mknod);
        int res = res_vnode->vn_ops->mknod(res_vnode, name, namelen, mode, devid);
        dbg(DBG_VFS, "EXIT\n");
        return res;
}

/* Use dir_namev() to find the vnode of the dir we want to make the new
 * directory in.  Then use lookup() to make sure it doesn't already exist.
 * Finally call the dir's mkdir vn_ops. Return what it returns.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        path already exists.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_mkdir(const char *path)
{
        dbg(DBG_VFS, "ENTER\n");
        if (!path) return -EINVAL;

        if (strlen(path) > MAXPATHLEN) return -ENAMETOOLONG;

        size_t namelen=0;
        const char *name=NULL;
        vnode_t *res_vnode;
        int ret = dir_namev(path, &namelen,&name,NULL,&res_vnode);
        if (ret < 0) {
                return ret;
        }
        if (!res_vnode) return -ENOENT;

        //make sure is directory
        if (!S_ISDIR(res_vnode->vn_mode)) {
                vput(res_vnode);
                return -ENOTDIR;
        }

        if (namelen > NAME_LEN) {
                vput(res_vnode);
                return -ENAMETOOLONG;
        }

        vnode_t *result;
        if (name) {
                KASSERT(res_vnode->vn_ops->lookup);
                int ret2 = lookup(res_vnode, name, namelen, &result);
                if (!ret2) {
                        vput(res_vnode);
                        vput(result);
                        return -EEXIST;
                }
                vnode_t *vn = result;
        }

        KASSERT(res_vnode->vn_ops->mkdir);
        int ret3 = res_vnode->vn_ops->mkdir(res_vnode, name, namelen);
        vput(res_vnode);
        dbg(DBG_VFS, "EXIT\n");
        return ret3;
}

/* Use dir_namev() to find the vnode of the directory containing the dir to be
 * removed. Then call the containing dir's rmdir v_op.  The rmdir v_op will
 * return an error if the dir to be removed does not exist or is not empty, so
 * you don't need to worry about that here. Return the value of the v_op,
 * or an error.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EINVAL
 *        path has "." as its final component.
 *      o ENOTEMPTY
 *        path has ".." as its final component.
 *      o ENOENT
 *        A directory component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_rmdir(const char *path)
{
        dbg(DBG_VFS, "ENTER\n");
        KASSERT(path);
        if (strlen(path) > MAXPATHLEN) {
                return -ENAMETOOLONG;
        }

        size_t namelen=0;
        const char *name;
        vnode_t *res_vnode;
        int ret = dir_namev(path, &namelen,&name,NULL,&res_vnode);
        if (ret < 0) return ret;

        if (!res_vnode) {
                return -ENOENT;
        }

        if (namelen > NAME_LEN) {
                vput(res_vnode);
                return -ENAMETOOLONG;
        }
        if (!S_ISDIR(res_vnode->vn_mode)) {
                vput(res_vnode);
                return -ENOTDIR;
        }
        if (!strcmp(name, ".")) {
                vput(res_vnode);
                return -EINVAL;
        }
        if (!strcmp(name, "..")) {
                vput(res_vnode);
                return -ENOTEMPTY;
        }

        if (!res_vnode->vn_ops->rmdir) {
            vput(res_vnode);
            return -ENOTDIR;
        }

        KASSERT(res_vnode->vn_ops->rmdir);
        int ret3 = res_vnode->vn_ops->rmdir(res_vnode, name, namelen);
        vput(res_vnode);
        dbg(DBG_VFS, "EXIT\n");
        return ret3;
}

/*
 * Same as do_rmdir, but for files.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EISDIR
 *        path refers to a directory.
 *      o ENOENT
 *        A component in path does not exist.
 *      o ENOTDIR
 *        A component used as a directory in path is not, in fact, a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_unlink(const char *path)
{
        dbg(DBG_VFS, "ENTER\n");

        if (!path) return -EINVAL;

        if (strlen(path) > MAXPATHLEN) {
                return -ENAMETOOLONG;
        }

        size_t namelen=0;
        const char *name;
        vnode_t *res_vnode;
        int ret = dir_namev(path, &namelen,&name,NULL,&res_vnode);

        if (ret < 0) return ret;


        if (namelen > NAME_LEN) {
                return -ENAMETOOLONG;
        }

        // THIS RUINED EVERYTHING
        // if (namelen > NAME_LEN) {
        //         vput(res_vnode);
        //         return -ENAMETOOLONG;
        // }

        vnode_t *result;
        ret = lookup(res_vnode, name, namelen, &result);
        if (ret < 0) {
            vput(res_vnode);
            return ret;
        }

        if (S_ISDIR(result->vn_mode)) {
            vput(res_vnode);
            vput(result);
            return -EPERM;//this was changed to pass two tests
        }

        ret = res_vnode->vn_ops->unlink(res_vnode, name, namelen);
        vput(res_vnode);
        vput(result);
        dbg(DBG_VFS, "EXIT\n");
        return ret;
}

/* To link:
 *      o open_namev(from)
 *      o dir_namev(to)
 *      o call the destination dir's (to) link vn_ops.
 *      o return the result of link, or an error
 *
 * Remember to vput the vnodes returned from open_namev and dir_namev.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EEXIST
 *        to already exists.
 *      o ENOENT
 *        A directory component in from or to does not exist.
 *      o ENOTDIR
 *        A component used as a directory in from or to is not, in fact, a
 *        directory.
 *      o ENAMETOOLONG
 *        A component of from or to was too long.
 *      o EPERM
 *        from is a directory.
 */
int
do_link(const char *from, const char *to)
{
        dbg(DBG_VFS, "ENTER\n");
        if (!from || !to) return -EINVAL;
        if (strlen(from) > MAXPATHLEN || strlen(to) > MAXPATHLEN) {
                return -ENAMETOOLONG;
        }

        size_t namelen = 0;
        const char *name = NULL;
        vnode_t *fromNode;
        int ret1 = open_namev(from, 0, &fromNode, NULL);
        if (ret1 < 0) {
            return ret1;
        }

        vnode_t *toNode;
        int ret2 = dir_namev(to, &namelen, &name, NULL, &toNode);
        if (ret2 < 0) {
            vput(fromNode);
            return ret2;
        }

        vnode_t *result;
        int ret3 = lookup(toNode, name, namelen, &result);
        if (!ret3) {
            vput(fromNode);
            vput(toNode);
            vput(result);
            return -EEXIST;
        }

        if (!toNode->vn_ops->link) {
            vput(fromNode);
            vput(toNode);
            return -ENOTDIR;
        }

        int ret4 = toNode->vn_ops->link(fromNode, toNode, name, namelen);
        vput(fromNode);
        vput(toNode);
        dbg(DBG_VFS, "EXIT\n");
        return ret4;
}

 

/*      o link newname to oldname
 *      o unlink oldname
 *      o return the value of unlink, or an error
 *
 * Note that this does not provide the same behavior as the
 * Linux system call (if unlink fails then two links to the
 * file could exist).
 */
int
do_rename(const char *oldname, const char *newname)
{
        dbg(DBG_VFS, "ENTER\n");
        KASSERT(oldname && newname);

        int ret = do_link(oldname, newname);
        if (ret < 0) return ret;

        size_t namelen = 0;
        const char *name = NULL;
        vnode_t *node;

        ret = dir_namev(oldname, &namelen, &name, NULL, &node);

        if (ret < 0) {
                vput(node);
                return ret;
        }
        vnode_t *result;
        if (name) {
                ret = lookup(node, name, namelen, &result);
                if (!ret) {
                        vput(result);
                        if(S_ISDIR(result->vn_mode)) {
                                ret = do_rmdir(oldname);
                        }
                        else ret = do_unlink(oldname);
                }
        }

        vput(node);
        dbg(DBG_VFS, "EXIT\n");
        return ret;
}

/* Make the named directory the current process's cwd (current working
 * directory).  Don't forget to down the refcount to the old cwd (vput()) and
 * up the refcount to the new cwd (open_namev() or vget()). Return 0 on
 * success.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        path does not exist.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 *      o ENOTDIR
 *        A component of path is not a directory.
 */
int
do_chdir(const char *path)
{
        dbg(DBG_VFS, "ENTER\n");
        if (!path) return -EINVAL;

        if (strlen(path) > MAXPATHLEN) return -ENAMETOOLONG;

        vnode_t *res_vnode;
        int ret = open_namev(path, NULL, &res_vnode, NULL);

        if (ret < 0) return ret;

        if (!res_vnode) return -ENOENT;

        if (!S_ISDIR(res_vnode->vn_mode)) {
                vput(res_vnode);
                return -ENOTDIR;
        }

        vput(curproc->p_cwd);
        curproc->p_cwd = res_vnode;
        dbg(DBG_VFS, "EXIT\n");
        return 0;
}

/* Call the readdir f_op on the given fd, filling in the given dirent_t*.
 * If the readdir f_op is successful, it will return a positive value which
 * is the number of bytes copied to the dirent_t.  You need to increment the
 * file_t's f_pos by this amount.  As always, be aware of refcounts, check
 * the return value of the fget and the virtual function, and be sure the
 * virtual function exists (is not null) before calling it.
 *
 * Return either 0 or sizeof(dirent_t), or -errno.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        Invalid file descriptor fd.
 *      o ENOTDIR
 *        File descriptor does not refer to a directory.
 */
int
do_getdent(int fd, struct dirent *dirp)
{
        dbg(DBG_VFS, "ENTER\n");
        KASSERT(dirp);

        if (fd < 0 || fd > NFILES || !curproc->p_files[fd]) {
                return -EBADF;
        }

        file_t *file = fget(fd);

        KASSERT(file);


        if (!S_ISDIR(file->f_vnode->vn_mode)) {
                fput(file);
                return -ENOTDIR;
        }

        int bytes=0;
        KASSERT(file->f_vnode->vn_ops->readdir);
        bytes = file->f_vnode->vn_ops->readdir(file->f_vnode, file->f_pos, dirp);
        file->f_pos = file->f_pos + bytes;
        if (bytes <= 0) {
                fput(file);
                return bytes;
        }

        
        fput(file);
        dbg(DBG_VFS, "EXIT\n");
        return sizeof(dirent_t);

}

/*
 * Modify f_pos according to offset and whence.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o EBADF
 *        fd is not an open file descriptor.
 *      o EINVAL
 *        whence is not one of SEEK_SET, SEEK_CUR, SEEK_END; or the resulting
 *        file offset would be negative.
 */
int
do_lseek(int fd, int offset, int whence)
{
        dbg(DBG_VFS, "ENTER\n");
        if (fd < 0 || fd > NFILES || (!curproc->p_files[fd])) {
                return -EBADF;
        }
        if ((whence != SEEK_SET) && (whence != SEEK_CUR) && (whence != SEEK_END)) {
                return -EINVAL;
        }

        file_t *file = fget(fd);

        KASSERT(file);

        if (whence == SEEK_SET) {
                if (offset < 0) {
                        fput(file);
                        return -EINVAL;
                }
                file->f_pos = (off_t) offset;
                fput(file);

        }
        if (whence == SEEK_CUR) {
                if (file->f_pos + offset < 0) {
                        fput(file);
                        return -EINVAL;
                }
                else {
                        file->f_pos = (off_t) file->f_pos + offset;
                        fput(file);
                }
        }
        if (whence == SEEK_END) {
                if (file->f_vnode->vn_len + offset < 0) {
                        fput(file);
                        return -EINVAL;
                }
                file->f_pos = (off_t) file->f_vnode->vn_len + offset;
                fput(file);
        }
        dbg(DBG_VFS, "EXIT\n");
        return file->f_pos;
}

/*
 * Find the vnode associated with the path, and call the stat() vnode operation.
 *
 * Error cases you must handle for this function at the VFS level:
 *      o ENOENT
 *        A component of path does not exist.
 *      o ENOTDIR
 *        A component of the path prefix of path is not a directory.
 *      o ENAMETOOLONG
 *        A component of path was too long.
 */
int
do_stat(const char *path, struct stat *buf)
{
        dbg(DBG_VFS, "ENTER\n");

        if (!buf || !path) return -EINVAL;

        if (strlen(path) > MAXPATHLEN) {
                return -ENAMETOOLONG;
        }

        size_t namelen = 0;
        const char *name = NULL;
        vnode_t *res_vnode;

        int ret = dir_namev(path, &namelen, &name, NULL, &res_vnode);
        //KASSERT(res_vnode);
        //dbg(DBG_VFS, "1\n");
        if (ret < 0) return ret;

        if (namelen > NAME_LEN) {
                vput(res_vnode);
                return -ENAMETOOLONG;
        }
        //dbg(DBG_VFS, "2\n");
        if (!res_vnode) {
                return -ENOENT;
        }
        else {
                //dbg(DBG_VFS, "3\n");
                if (!S_ISDIR(res_vnode->vn_mode)) {
                        vput(res_vnode);
                        //dbg(DBG_VFS, "4\n");
                        return -ENOTDIR;
                }
        }

        vnode_t *result;
        if (name) {
                //dbg(DBG_VFS, "5\n");

                ret = lookup(res_vnode, name, strlen(name), &result);
                ///dbg(DBG_VFS, "5.5\n");
                if (ret) {
                        //dbg(DBG_VFS, "6\n");
                        vput(res_vnode);
                        return ret;
                }
        }

        if (!result) {
                //dbg(DBG_VFS, "7\n");
                vput(res_vnode);
                return -ENOENT;
        }

        //dbg(DBG_VFS, "8\n");
        KASSERT(res_vnode->vn_ops->stat);
        ret = res_vnode->vn_ops->stat(result, buf);

        vput(res_vnode);
        vput(result);
        dbg(DBG_CORE, "EXIT\n");
        return ret;
}

#ifdef __MOUNTING__
/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutely sure your Weenix is perfect.
 *
 * This is the syscall entry point into vfs for mounting. You will need to
 * create the fs_t struct and populate its fs_dev and fs_type fields before
 * calling vfs's mountfunc(). mountfunc() will use the fields you populated
 * in order to determine which underlying filesystem's mount function should
 * be run, then it will finish setting up the fs_t struct. At this point you
 * have a fully functioning file system, however it is not mounted on the
 * virtual file system, you will need to call vfs_mount to do this.
 *
 * There are lots of things which can go wrong here. Make sure you have good
 * error handling. Remember the fs_dev and fs_type buffers have limited size
 * so you should not write arbitrary length strings to them.
 */
int
do_mount(const char *source, const char *target, const char *type)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_mount");
        return -EINVAL;
}

/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutley sure your Weenix is perfect.
 *
 * This function delegates all of the real work to vfs_umount. You should not worry
 * about freeing the fs_t struct here, that is done in vfs_umount. All this function
 * does is figure out which file system to pass to vfs_umount and do good error
 * checking.
 */
int
do_umount(const char *target)
{
        NOT_YET_IMPLEMENTED("MOUNTING: do_umount");
        return -EINVAL;
}
#endif
