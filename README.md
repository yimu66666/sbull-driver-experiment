# SBULL 设备驱动实验

这是一个模拟的块设备驱动程序实验。

## 功能
- 实现一个虚拟的块设备
- 支持基本的磁盘操作（格式化、挂载、读写）
- 使用内核调试器监控操作

## 编译
make

## 安装
sudo insmod sbull.ko

## 卸载
sudo rmmod sbull.ko
# Development branch for sbull driver
## Testing Procedures
1. Compile with 'make'
2. Load module with 'sudo insmod sbull.ko'
