#include <linux/init.h>    // __init, __exit
#include <linux/module.h>  // module_init, module_exit, MODULE_LICENSE
#include <linux/fs.h>      // struct file_system_type, struct inode, new_inode
#include <linux/mount.h>   // mount_nodev, kill_anon_super
#include <linux/kernel.h>  // printk, pr_info
#include <linux/slab.h>    // kmalloc, kfree
#include <linux/uaccess.h> // copy_to_user, copy_from_user

#define MODULE_NAME "vtfs"

#define MAX_CHILDREN  64
#define MAX_NAME_LEN  255
#define MAX_FILE_SIZE 4096

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

/* ============================================================================
 * In-memory tree node
 * ==========================================================================*/

typedef struct vtfs_child {
    char name[MAX_NAME_LEN];
    struct vtfs_node *node;
} vtfs_child_t;

typedef struct vtfs_node {
	ino_t ino;
	umode_t mode;               // S_IFREG or S_IFDIR (+ perms)
	char *data;                 // file content (NULL for dirs)
	size_t data_size;
	unsigned int nlink;

	vtfs_child_t children[MAX_CHILDREN];
	int child_count;

	struct vtfs_node *parent;
} vtfs_node_t;

/* ============================================================================
 * Forward declarations
 * ==========================================================================*/

static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode,
                                    vtfs_node_t *node);

static int vtfs_create(struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode,
                       bool b);

static int vtfs_unlink(struct inode *parent_inode,
                       struct dentry *child_dentry);

static struct dentry *vtfs_lookup(struct inode *parent_inode,
                                  struct dentry *child_dentry,
                                  unsigned int flags);

static int vtfs_iterate(struct file *filp, struct dir_context *ctx);

static int vtfs_mkdir(struct inode *parent_inode,
                      struct dentry *child_dentry,
                      umode_t mode);

static int vtfs_rmdir(struct inode *parent_inode,
                      struct dentry *child_dentry);

static ssize_t vtfs_read(struct file *filp,
                         char __user *buffer,
                         size_t len,
                         loff_t *offset);

static ssize_t vtfs_write(struct file *filp,
                          const char __user *buffer,
                          size_t len,
                          loff_t *offset);

static int vtfs_link(struct dentry *old_dentry,
                     struct inode *parent_dir,
                     struct dentry *new_dentry);

static int vtfs_fill_super(struct super_block *sb, void *data, int silent);

static struct dentry *vtfs_mount(struct file_system_type *fs_type,
                                 int flags,
                                 const char *token,
                                 void *data);

static void vtfs_kill_sb(struct super_block *sb);

/* ============================================================================
 * Global state
 * ==========================================================================*/

static atomic_t next_ino = ATOMIC_INIT(1001);

/* ============================================================================
 * RAM FS helpers
 * ==========================================================================*/

static vtfs_node_t *vtfs_fs_init_root(void)
{
	vtfs_node_t *root = kmalloc(sizeof(*root), GFP_KERNEL);
	if (!root)
		return NULL;

	root->ino = 1000;

	root->mode = S_IFDIR | 0777;
	root->data = NULL;
	root->data_size = 0;
	root->nlink = 1;

	root->child_count = 0;
	root->parent = NULL;

	return root;
}

static vtfs_node_t *vtfs_fs_lookup(vtfs_node_t *parent, const char *name)
{
	int i;

	for (i = 0; i < parent->child_count; i++) {
		if (!strcmp(parent->children[i].name, name))
			return parent->children[i].node;
	}

	return NULL;
}

static vtfs_node_t *vtfs_fs_create_node(vtfs_node_t *parent,
                                        const char *name,
                                        umode_t mode)
{
	vtfs_node_t *node;

	if (parent->child_count >= MAX_CHILDREN)
		return NULL;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;

	node->ino = atomic_inc_return(&next_ino);

	node->mode = mode;
	node->data = NULL;
	node->data_size = 0;
	node->nlink = 1;

	node->child_count = 0;
	node->parent = parent;

	strncpy(parent->children[parent->child_count].name, name, MAX_NAME_LEN - 1);
    parent->children[parent->child_count].name[MAX_NAME_LEN - 1] = '\0';
    parent->children[parent->child_count].node = node;
    parent->child_count++;

	return node;
}

static int vtfs_fs_delete_node(vtfs_node_t *parent, const char *name)
{
	int i;
	vtfs_node_t *node;

	for (i = 0; i < parent->child_count; i++) {
		if (!strcmp(parent->children[i].name, name)) {
			node = parent->children[i].node;

			/* Shift remaining children left */
			for (; i < parent->child_count - 1; i++)
				parent->children[i] = parent->children[i + 1];

			parent->child_count--;

			node->nlink--;
			if (node->nlink == 0) {
				kfree(node->data);
				kfree(node);
			}

			return 0;
		}
	}

	return -ENOENT;
}

static void vtfs_fs_destroy(vtfs_node_t *node)
{
	int i;

	if (!node)
		return;

	for (i = 0; i < node->child_count; i++)
		vtfs_fs_destroy(node->children[i].node);

	if (node->nlink > 1) {
		node->nlink--;
		return;
	}

	kfree(node->data);
	kfree(node);
}

/* ============================================================================
 * VFS inode / file operations
 * ==========================================================================*/

static const struct inode_operations vtfs_inode_ops = {
	.create = vtfs_create,
	.unlink = vtfs_unlink,
	.lookup = vtfs_lookup,
	.mkdir	= vtfs_mkdir,
	.rmdir	= vtfs_rmdir,
	.link	= vtfs_link,
};

static const struct file_operations vtfs_file_ops = {
    .read  	= vtfs_read,
    .write	= vtfs_write,
};

static const struct file_operations vtfs_dir_ops = {
	.iterate = vtfs_iterate,
};

static int vtfs_create(struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode,
                       bool b)
{
	struct inode *inode;
	vtfs_node_t *parent;
	vtfs_node_t *new_node;
	const char *name;

	parent = parent_inode->i_private;
	name = child_dentry->d_name.name;

	new_node = vtfs_fs_create_node(parent, name, S_IFREG | 0777);
	if (!new_node)
		return -ENOMEM;

	inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | 0777, new_node);
	if (!inode)
		return -ENOMEM;

	d_instantiate(child_dentry, inode);
	return 0;
}

static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry)
{
	vtfs_node_t *parent;
	int err;

	parent = parent_inode->i_private;
	err = vtfs_fs_delete_node(parent, child_dentry->d_name.name);
	if (err)
		return err;
	
	drop_nlink(child_dentry->d_inode);
	
	return 0;	
}

static struct dentry *vtfs_lookup(struct inode *parent_inode,
                                  struct dentry *child_dentry,
                                  unsigned int flags)
{
	vtfs_node_t *parent;
	vtfs_node_t *found;

	parent = parent_inode->i_private;
	found = vtfs_fs_lookup(parent, child_dentry->d_name.name);

	if (found) {
		struct inode *inode =
			vtfs_get_inode(parent_inode->i_sb, NULL, found->mode, found);
		d_add(child_dentry, inode);
	}

	return NULL;
}

static int vtfs_iterate(struct file *filp, struct dir_context *ctx)
{
	vtfs_node_t *dir;
	int i;

	dir = filp->f_path.dentry->d_inode->i_private;

	if (!dir_emit_dots(filp, ctx))
		return 0;

	i = (int)ctx->pos - 2;
	while (i < dir->child_count) {
		vtfs_child_t child = dir->children[i];
		unsigned char ftype = S_ISDIR(child.node->mode) ? DT_DIR : DT_REG;

		if (!dir_emit(ctx, child.name, strlen(child.name), child.node->ino, ftype))
			return 0;

		ctx->pos++;
		i++;
	}

	return 0;
}

static int vtfs_mkdir(struct inode *parent_inode,
                      struct dentry *child_dentry,
                      umode_t mode)
{
    vtfs_node_t *parent;
    const char  *name;
    vtfs_node_t *new_node;
    struct inode *inode;

    parent = parent_inode->i_private;
    name = child_dentry->d_name.name;

    new_node = vtfs_fs_create_node(parent, name, S_IFDIR | 0777);
    if (!new_node)
        return -ENOMEM;

    inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFDIR | 0777, new_node);
    if (!inode)
        return -ENOMEM;

    inc_nlink(inode);
    inc_nlink(parent_inode);

    d_instantiate(child_dentry, inode);
    return 0;
}

static int vtfs_rmdir(struct inode *parent_inode, struct dentry *child_dentry)
{
    vtfs_node_t *parent;
    vtfs_node_t *node;

    parent = parent_inode->i_private;
    node = vtfs_fs_lookup(parent, child_dentry->d_name.name);
    
    if (!node)
        return -ENOENT;

    if (node->child_count > 0)
        return -ENOTEMPTY;
	
    drop_nlink(child_dentry->d_inode);   // undo the "." link
    drop_nlink(child_dentry->d_inode);   // undo the entry in parent
    drop_nlink(parent_inode);            // undo the ".." back-reference

    return vtfs_fs_delete_node(parent, child_dentry->d_name.name);
}

static ssize_t vtfs_read(struct file *filp,
                         char __user *buffer,
                         size_t len,
                         loff_t *offset)
{
    vtfs_node_t *node;
    ssize_t remaining;

	node = filp->f_path.dentry->d_inode->i_private;

    if (*offset >= node->data_size)
        return 0;

    remaining = node->data_size - *offset;
    if (len > remaining)
        len = remaining;

    if (copy_to_user(buffer, node->data + *offset, len))
        return -EFAULT;

    *offset += len;
    return len;
}

static ssize_t vtfs_write(struct file *filp,
                          const char __user *buffer,
                          size_t len,
                          loff_t *offset)
{
    vtfs_node_t *node;
    size_t new_size;
    char *new_data;

	node = filp->f_path.dentry->d_inode->i_private;
	new_size = *offset + len;

    if (new_size > MAX_FILE_SIZE)
        return -ENOSPC;

    new_data = krealloc(node->data, new_size, GFP_KERNEL);
    if (!new_data)
        return -ENOMEM;

    node->data = new_data;

    if (copy_from_user(node->data + *offset, buffer, len))
        return -EFAULT;

    node->data_size = new_size;
    *offset += len;

    filp->f_path.dentry->d_inode->i_size = new_size;

    return len;
}

static int vtfs_link(struct dentry *old_dentry,
                     struct inode *parent_dir,
                     struct dentry *new_dentry)
{
    struct inode *inode;
    vtfs_node_t  *node;
    vtfs_node_t  *new_parent;

	inode = old_dentry->d_inode;
	node = inode->i_private;
	new_parent = parent_dir->i_private;

    if (S_ISDIR(inode->i_mode))
        return -EPERM;

    if (new_parent->child_count >= MAX_CHILDREN)
        return -ENOSPC;

    strncpy(new_parent->children[new_parent->child_count].name, 
			new_dentry->d_name.name, MAX_NAME_LEN - 1);
    new_parent->children[new_parent->child_count].name[MAX_NAME_LEN - 1] = '\0';
    new_parent->children[new_parent->child_count].node = node;
	new_parent->child_count++;

    node->nlink++;
    inc_nlink(inode);
    ihold(inode);
	d_drop(new_dentry);
	d_add(new_dentry, inode);

    return 0;
}

/* ============================================================================
 * Superblock / mount
 * ==========================================================================*/

static void vtfs_kill_sb(struct super_block *sb)
{
	vtfs_fs_destroy(sb->s_fs_info);
	kill_anon_super(sb);
}

static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode,
                                    vtfs_node_t *node)
{
	struct inode *inode = iget_locked(sb, node->ino);
	if (!inode)
		return NULL;

	if (inode->i_state & I_NEW) {
		inode_init_owner(inode, dir, mode);

		inode->i_private = node;
		inode->i_ino = node->ino;
		inode->i_op = &vtfs_inode_ops;

		if (S_ISDIR(inode->i_mode))
			inode->i_fop = &vtfs_dir_ops;
		else
			inode->i_fop = &vtfs_file_ops;

		inode->i_size = node->data_size;
        set_nlink(inode, node->nlink);
        unlock_new_inode(inode);
	}

	return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;
	vtfs_node_t *root_node;

	root_node = vtfs_fs_init_root();
	if (!root_node)
		return -ENOMEM;

	sb->s_fs_info = root_node;

	inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, root_node);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		vtfs_fs_destroy(root_node);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}

	return 0;
}

static struct dentry *vtfs_mount(struct file_system_type *fs_type,
                                 int flags,
                                 const char *token,
                                 void *data)
{
	struct dentry *ret;

	ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
	if (!ret)
		printk(KERN_ERR "Can't mount file system\n");
	else
		printk(KERN_INFO "Mounted successfully\n");

	return ret;
}

static struct file_system_type vtfs_fs_type = {
	.name    = "vtfs",
	.mount   = vtfs_mount,
	.kill_sb = vtfs_kill_sb,
};

/* ============================================================================
 * Module init/exit
 * ==========================================================================*/

static int __init vtfs_init(void)
{
	int err;

	err = register_filesystem(&vtfs_fs_type);
	if (err) {
		LOG("Failed to register filesystem: %d\n", err);
		return err;
	}

	LOG("VTFS joined the kernel\n");
	return 0;
}

static void __exit vtfs_exit(void)
{
	unregister_filesystem(&vtfs_fs_type);
	LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
