#include "errno.h"
#include "globals.h"
#include "kernel.h"
#include <fs/dirent.h>

#include "util/debug.h"
#include "util/string.h"

#include "fs/fcntl.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"

/*
 * Get the parent of a directory. dir must not be locked.
 */
long namev_get_parent(vnode_t *dir, vnode_t **out)
{
    vlock(dir);
    long ret = namev_lookup(dir, "..", 2, out);
    vunlock(dir);
    return ret;
}

/*
 * Determines if vnode a is a descendant of vnode b.
 * Returns 1 if true, 0 otherwise.
 */
long namev_is_descendant(vnode_t *a, vnode_t *b)
{
    vref(a);
    vnode_t *cur = a;
    vnode_t *next = NULL;
    while (cur != NULL)
    {
        if (cur->vn_vno == b->vn_vno)
        {
            vput(&cur);
            return 1;
        }
        else if (cur->vn_vno == cur->vn_fs->fs_root->vn_vno)
        {
            /* we've reached the root node. */
            vput(&cur);
            return 0;
        }

        /* backup the filesystem tree */
        namev_get_parent(cur, &next);
        vnode_t *tmp = cur;
        cur = next;
        vput(&tmp);
    }

    return 0;
}

/* Wrapper around dir's vnode operation lookup. dir must be locked on entry and
 *  upon return.
 *
 * Upon success, return 0 and return the found vnode using res_vnode, or:
 *  - ENOTDIR: dir does not have a lookup operation or is not a directory
 *  - Propagate errors from the vnode operation lookup
 *
 * Hints:
 * Take a look at ramfs_lookup(), which adds a reference to res_vnode but does
 * not touch any locks. In most cases, this means res_vnode will be unlocked
 * upon return. However, there is a case where res_vnode would actually be
 * locked after calling dir's lookup function (i.e. looking up '.'). You
 * shouldn't deal with any locking in namev_lookup(), but you should be aware of
 * this special case when writing other functions that use namev_lookup().
 * Because you are the one writing nearly all of the calls to namev_lookup(), it
 * is up to you both how you handle all inputs (i.e. dir or name is null,
 * namelen is 0), and whether namev_lookup() even gets called with a bad input.
 */
long namev_lookup(vnode_t *dir, const char *name, size_t namelen,
                  vnode_t **res_vnode)
{
    KASSERT(dir->vn_mobj.mo_mutex.km_holder != NULL);

    if (!S_ISDIR(dir->vn_mode) || dir->vn_ops == NULL || dir->vn_ops->lookup == NULL)
    {
        return -ENOTDIR;
    }

    return dir->vn_ops->lookup(dir, name, namelen, res_vnode);
}

/*
 * Find the next meaningful token in a string representing a path.
 *
 * Returns the token and sets `len` to be the token's length.
 *
 * Once all tokens have been returned, the next char* returned is either NULL
 * 	or "" (the empty string). In order to handle both, if you're calling 
 * 	this in a loop, we suggest terminating the loop once the value returned
 * 	in len is 0
 * 
 * Example usage: t
 * - "/dev/null" 
 * ==> *search would point to the first character of "/null"
 * ==> *len would be 3 (as "dev" is of length 3)
 * ==> namev_tokenize would return a pointer to the 
 *     first character of "dev/null"
 * 
 * - "a/b/c"
 * ==> *search would point to the first character of "/b/c"
 * ==> *len would be 1 (as "a" is of length 1)
 * ==> namev_tokenize would return a pointer to the first character
 *     of "a/b/c"
 * 
 * We highly suggest testing this function outside of Weenix; for instance
 * using an online compiler or compiling and testing locally to fully 
 * understand its behavior. See handout for an example. 
 */
static const char *namev_tokenize(const char **search, size_t *len)
{
    const char *begin;

    if (*search == NULL)
    {
        *len = 0;
        return NULL;
    }

    KASSERT(NULL != *search);

    /* Skip initial '/' to find the beginning of the token. */
    while (**search == '/')
    {
        (*search)++;
    }

    /* Determine the length of the token by searching for either the
     *  next '/' or the end of the path. */
    begin = *search;
    *len = 0;
    while (**search && **search != '/')
    {
        (*len)++;
        (*search)++;
    }

    if (!**search)
    {
        *search = NULL;
    }

    return begin;
}

/*
 * Parse path and return in `res_vnode` the vnode corresponding to the directory
 * containing the basename (last element) of path. `base` must not be locked on
 * entry or on return. `res_vnode` must not be locked on return. Return via `name`
 * and `namelen` the basename of path.
 *
 * Return 0 on success, or:
 *  - EINVAL: path refers to an empty string
 *  - Propagate errors from namev_lookup()
 *
 * Hints:
 *  - When *calling* namev_dir(), if it is unclear what to pass as the `base`, you
 *    should use `curproc->p_cwd` (think about why this makes sense).
 *  - `curproc` is a global variable that represents the current running process 
 *    (a proc_t struct), which has a field called p_cwd. 
 *  - The first parameter, base, is the vnode from which to start resolving
 *    path, unless path starts with a '/', in which case you should start at
 *    the root vnode, vfs_root_fs.fs_root.
 *  - Use namev_lookup() to handle each individual lookup. When looping, be
 *    careful about locking and refcounts, and make sure to clean up properly
 *    upon failure.
 *  - namev_lookup() should return with the found vnode unlocked, unless the
 *    found vnode is the same as the given directory (e.g. "/./."). Be mindful
 *    of this special case, and any locking/refcounting that comes with it.
 *  - When parsing the path, you do not need to implement hand-over-hand
 *    locking. That is, when calling `namev_lookup(dir, path, pathlen, &out)`,
 *    it is safe to put away and unlock dir before locking `out`.
 *  - You are encouraged to use namev_tokenize() to help parse path.
 *  - Whether you're using the provided base or the root vnode, you will have
 *    to explicitly lock and reference your starting vnode before using it.
 *  - Don't allocate memory to return name. Just set name to point into the
 *    correct part of path.
 *
 * Example usage:
 *  - "/a/.././//b/ccc/" ==> res_vnode = vnode for b, name = "ccc", namelen = 3
 *  - "tmp/..//." ==> res_vnode = base, name = ".", namelen = 1
 *  - "/dev/null" ==> rev_vnode = vnode for /dev, name = "null", namelen = 4
 * For more examples of expected behavior, you can try out the command line
 * utilities `dirname` and `basename` on your virtual machine or a Brown
 * department machine.
 */
long namev_dir(vnode_t *base, const char *path, vnode_t **res_vnode,
               const char **name, size_t *namelen)
{
    KASSERT(base->vn_mobj.mo_mutex.km_holder == NULL);

    if (*path == '\0')
    {
        return -EINVAL;
    }

    // check if path starts with "/", lock and reference base
    if (*path == '/')
    {
        base = vfs_root_fs.fs_root;
    }

    vref(base);
    size_t tmp_namelen;
    vnode_t *tmp_res_vnode = base;
    *res_vnode = base;
    const char *tmp_name = namev_tokenize(&path, &tmp_namelen);
    while (tmp_namelen)
    {
        *name = tmp_name;
        *namelen = tmp_namelen;
        base = tmp_res_vnode;
        *res_vnode = tmp_res_vnode;
        vlock(base);
        // call to lookup, unlock dir, error check
        long ret = namev_lookup(base, *name, *namelen, &tmp_res_vnode);
        vunlock(base);
        tmp_name = namev_tokenize(&path, &tmp_namelen);
        if (ret)
        {
            KASSERT((*res_vnode)->vn_mobj.mo_mutex.km_holder == NULL);
            if (ret == -ENOENT && !tmp_namelen)
            {
                return 0;
            }
            vput(&base);
            return ret;
        }
        else
        {
            // if (/*base != tmp_res_vnode &&*/ tmp_namelen)
            // {
                
            // }
            if (!tmp_namelen) {
                vput(&tmp_res_vnode);
            } else {
                vput(&base);
            }
        }
    }
    KASSERT((*res_vnode)->vn_mobj.mo_mutex.km_holder == NULL);
    return 0;


    // // error check for empty path
    // if (*path == '\0') {
    //     return -EINVAL;
    // }
    // // check if path starts with "/", lock and reference base
    // if (*path == '/') {
    //     base = vfs_root_fs.fs_root;
    // }

    // vnode_t *curr = base;
    // vnode_t *next = base;
    // *res_vnode = NULL;
    // long ret;
    // vref(curr);
    // while (1) {
    //     // parse the next token
    //     size_t tmp_len = 0;
    //     const char *tmp_name = namev_tokenize(&path, &tmp_len);
    //     if (tmp_len <= 0) {
    //         break;
    //     }
    //     // if previous lookup failed, return with no net change in ref count
    //     if (ret == -ENOENT) {
    //         return ret;
    //     }

    //     *res_vnode = next;
    //     *name = tmp_name;
    //     *namelen = tmp_len;
    //     curr = next;
    //     next = NULL;
    //     // lock and perform lookup
    //     vlock(curr);
    //     ret = namev_lookup(curr, tmp_name, tmp_len, &next);
    //     vunlock(curr);
    //     // if other error found, return with no net change in ref count
    //     if (ret && ret != -ENOENT) {
    //         vput(&curr);
    //         return ret;
    //     }
    //     if (curr != next) {
    //         vput(&curr);
    //     }
    // }
    // if (!(*res_vnode)) {
    //     *res_vnode = next;
    // }
    // return 0;
}

/*
 * Open the file specified by `base` and `path`, or create it, if necessary.
 *  Return the file's vnode via `res_vnode`, which should be returned unlocked
 *  and with an added reference.
 *
 * Return 0 on success, or:
 *  - EINVAL: O_CREAT is specified but path implies a directory
 *  - ENAMETOOLONG: path basename is too long
 *  - ENOTDIR: Attempting to open a regular file as a directory
 *  - Propagate errors from namev_dir() and namev_lookup()
 *
 * Hints:
 *  - A path ending in '/' implies that the basename is a directory.
 *  - Use namev_dir() to get the directory containing the basename.
 *  - Use namev_lookup() to try to obtain the desired vnode.
 *  - If namev_lookup() fails and O_CREAT is specified in oflags, use
 *    the parent directory's vnode operation mknod to create the vnode.
 *    Use the basename info from namev_dir(), and the mode and devid
 *    provided to namev_open().
 *  - Use the macro S_ISDIR() to check if a vnode actually is a directory.
 *  - Use the macro NAME_LEN to check the basename length. Check out
 *    ramfs_mknod() to confirm that the name should be null-terminated.
 */
long namev_open(vnode_t *base, const char *path, int oflags, int mode,
                devid_t devid, struct vnode **res_vnode)
{
    int isDir = path[strlen(path) - 1] == '/';
    int doCreat = oflags & O_CREAT;

    if (doCreat && isDir)
    {
        return -EINVAL;
    }

    // find the base node and error check
    const char *basename;
    size_t basenamelen = 0;
    long ret = namev_dir(base, path, res_vnode, &basename, &basenamelen);
    if (ret)
    {
        return ret;
    }
    if (basenamelen > NAME_LEN)
    {
        vput(res_vnode);
        return -ENAMETOOLONG;
    }

    // lookup the target node
    vnode_t *basedir = *res_vnode;
    vlock(basedir);
    ret = namev_lookup(basedir, basename, basenamelen, res_vnode);
    // error check lookup
    if (ret)
    {
        // try creating the file
        if (doCreat && ret == -ENOENT)
        {
            // basename[basenamelen] = '\0';
            ret = basedir->vn_ops->mknod(basedir, basename, basenamelen, mode, devid, res_vnode);
            vput_locked(&basedir);
            return ret;
        }
        vput_locked(&basedir);
        return ret;
    }
    vput_locked(&basedir);
    if (S_ISREG((*res_vnode)->vn_mode) && isDir) {
        vput(res_vnode);
        return -ENOTDIR;
    }
    return 0;
}

/*
 * Wrapper around namev_open with O_RDONLY and 0 mode/devid
 */
long namev_resolve(vnode_t *base, const char *path, vnode_t **res_vnode)
{
    return namev_open(base, path, O_RDONLY, 0, 0, res_vnode);
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
int lookup_name(vnode_t *dir, vnode_t *entry, char *buf, size_t size)
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
ssize_t lookup_dirpath(vnode_t *dir, char *buf, size_t osize)
{
    NOT_YET_IMPLEMENTED("GETCWD: lookup_dirpath");

    return -ENOENT;
}
#endif /* __GETCWD__ */
