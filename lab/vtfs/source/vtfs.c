#include <linux/init.h>           // For __init and __exit macros
#include <linux/module.h>         // For module_init, module_exit, and MODULE_LICENSE
#include <linux/fs.h>             // For struct file_system_type, struct inode, and alloc_anon_inode
#include <linux/mount.h>          // For mount related structures (if needed)
#include <linux/kernel.h>         // For printk and basic kernel types

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

struct dentry* vtfs_lookup(
  struct inode* parent_inode,  // родительская нода
  struct dentry* child_dentry, // объект, к которому мы пытаемся получить доступ
  unsigned int flag            // неиспользуемое значение
) {
  return 0;
}

struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
};

static int vtfs_iterate(struct file *filp, struct dir_context *ctx)
{
    if (!dir_emit_dots(filp, ctx))
        return 0;

    if (ctx->pos == 2) {
        if (!dir_emit(ctx, "test.txt", strlen("test.txt"), 101, DT_REG))
            return 0;
        ctx->pos++;
    }
    return 0;
}

struct file_operations vtfs_dir_ops = {
  .iterate = vtfs_iterate,
};

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "vtfs super block is destroyed. Unmount successfully.\n");
}

struct inode* vtfs_get_inode(
  struct super_block* sb, 
  const struct inode* dir, 
  umode_t mode, 
  int i_ino
) {
  struct inode *inode = new_inode(sb);
  if (inode != NULL) {
    inode_init_owner(inode, dir, mode);
  }
  inode->i_op = &vtfs_inode_ops;
  if (S_ISDIR(inode->i_mode))
    inode->i_fop = &vtfs_dir_ops;
  inode->i_ino = i_ino;
  return inode;
}

int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 1000);

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }

  printk(KERN_INFO "return 0\n");
  return 0;
}

struct dentry* mount_nodev(
  struct file_system_type* fs_type,
  int flags, 
  void* data, 
  int (*fill_super)(struct super_block*, void*, int)
);

struct dentry* vtfs_mount(
  struct file_system_type* fs_type,
  int flags,
  const char* token,
  void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    printk(KERN_ERR "Can't mount file system");
  } else {
    printk(KERN_INFO "Mounted successfuly");
  }
  return ret;
}

struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void) {
  int err = register_filesystem(&vtfs_fs_type);
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  unregister_filesystem(&vtfs_fs_type);
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
