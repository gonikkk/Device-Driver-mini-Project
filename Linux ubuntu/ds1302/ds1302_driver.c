#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>

#define DRIVER_NAME "ds1302_driver"
#define CLASS_NAME  "rtc_class"

/* 핀 설정 (GPIO 17, 27, 22) */
#define DS1302_CLK  17
#define DS1302_DAT  27
#define DS1302_RST  22

/* DS1302 명령 코드 */
#define CMD_READ_BURST   0xBF
#define CMD_WRITE_BURST  0xBE
#define CMD_WRITE_WP     0x8E // Write Protect

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;

/* BCD 변환 헬퍼 함수 */
static uint8_t bcd2bin(uint8_t val) { return ((val >> 4) * 10) + (val & 0x0F); }
static uint8_t bin2bcd(uint8_t val) { return ((val / 10) << 4) + (val % 10); }

/* ---- Low Level Bit-Banging 함수들 ---- */

/* 1바이트 전송 (LSB부터) */
static void ds1302_write_byte(uint8_t dat)
{
    int i;

    gpio_direction_output(DS1302_DAT, 0);

    for (i = 0; i < 8; i++) {
        gpio_set_value(DS1302_DAT, dat & 0x01);
        udelay(2);

        gpio_set_value(DS1302_CLK, 1);
        udelay(2);

        gpio_set_value(DS1302_CLK, 0);
        udelay(2);

        dat >>= 1;
    }
}

/* 1바이트 수신 (LSB부터) - ✅ 정석 구현 */
static uint8_t ds1302_read_byte(void)
{
    int i;
    uint8_t dat = 0;

    gpio_direction_input(DS1302_DAT);

    for (i = 0; i < 8; i++) {
        /* DS1302는 LSB-first: i번째 비트를 그대로 채움 */
        if (gpio_get_value(DS1302_DAT))
            dat |= (1 << i);

        gpio_set_value(DS1302_CLK, 1);
        udelay(2);
        gpio_set_value(DS1302_CLK, 0);
        udelay(2);
    }

    return dat;
}

/* 시간 읽기 함수 (Burst Mode 사용) */
static void ds1302_read_time(uint8_t *buf)
{
    int i;

    gpio_set_value(DS1302_RST, 1);
    udelay(4);

    ds1302_write_byte(CMD_READ_BURST);

    for (i = 0; i < 7; i++)
        buf[i] = ds1302_read_byte();

    gpio_set_value(DS1302_RST, 0);
    udelay(4);
}

/* 시간 쓰기 함수 */
static void ds1302_set_time(uint8_t *buf)
{
    int i;

    /* ✅ seconds CH bit clear 보장 */
    buf[0] &= 0x7F;

    /* 1) Write Protect Off */
    gpio_set_value(DS1302_RST, 1);
    udelay(4);
    ds1302_write_byte(CMD_WRITE_WP);
    ds1302_write_byte(0x00);
    gpio_set_value(DS1302_RST, 0);
    udelay(4);

    /* 2) Burst write */
    gpio_set_value(DS1302_RST, 1);
    udelay(4);
    ds1302_write_byte(CMD_WRITE_BURST);
    for (i = 0; i < 7; i++)
        ds1302_write_byte(buf[i]);

    ds1302_write_byte(0x00); // WP reg
    gpio_set_value(DS1302_RST, 0);
    udelay(4);
}

/* ---- File Operations ---- */

static int ds1302_open(struct inode *inode, struct file *file) { return 0; }
static int ds1302_release(struct inode *inode, struct file *file) { return 0; }

/* cat /dev/ds1302_driver */
static ssize_t ds1302_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    uint8_t time_reg[8] = {0};
    char msg[64];
    int len;

    ds1302_read_time(time_reg);

    /* ✅ seconds에서 CH bit 제거 후 변환 */
    time_reg[0] &= 0x7F;

    len = snprintf(msg, sizeof(msg), "20%02d-%02d-%02d %02d:%02d:%02d\n",
        bcd2bin(time_reg[6]),
        bcd2bin(time_reg[4]),
        bcd2bin(time_reg[3]),
        bcd2bin(time_reg[2]),
        bcd2bin(time_reg[1]),
        bcd2bin(time_reg[0])
    );

    return simple_read_from_buffer(buf, count, ppos, msg, len);
}

/* echo "24 12 25 13 00 00 3" > /dev/ds1302_driver */
static ssize_t ds1302_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[64];
    uint8_t time_reg[8];
    int year, month, day, hour, min, sec, wday;

    if (count > sizeof(kbuf) - 1) return -EINVAL;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;
    kbuf[count] = '\0';

    if (sscanf(kbuf, "%d %d %d %d %d %d %d",
               &year, &month, &day, &hour, &min, &sec, &wday) != 7) {
        printk("Invalid format. Use: YY MM DD HH MM SS WD\n");
        return -EINVAL;
    }

    time_reg[0] = bin2bcd(sec) & 0x7F;   /* ✅ CH=0 보장 */
    time_reg[1] = bin2bcd(min);
    time_reg[2] = bin2bcd(hour);
    time_reg[3] = bin2bcd(day);
    time_reg[4] = bin2bcd(month);
    time_reg[5] = bin2bcd(wday);
    time_reg[6] = bin2bcd(year);
    time_reg[7] = 0x00;

    ds1302_set_time(time_reg);
    return count;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = ds1302_open,
    .release = ds1302_release,
    .read    = ds1302_read,
    .write   = ds1302_write,
    .llseek  = default_llseek,
};

static int __init ds1302_init(void)
{
    if (gpio_request(DS1302_CLK, "ds1302_clk") ||
        gpio_request(DS1302_DAT, "ds1302_dat") ||
        gpio_request(DS1302_RST, "ds1302_rst")) {
        printk("DS1302: GPIO Request Failed\n");
        return -1;
    }

    gpio_direction_output(DS1302_CLK, 0);
    gpio_direction_output(DS1302_RST, 0);
    gpio_direction_output(DS1302_DAT, 0);

    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0)
        return -1;

    cdev_init(&my_cdev, &fops);
    cdev_add(&my_cdev, dev_num, 1);

    my_class = class_create(THIS_MODULE, CLASS_NAME);
    device_create(my_class, NULL, dev_num, NULL, DRIVER_NAME);

    printk("DS1302 Driver Initialized (GPIO %d,%d,%d)\n", DS1302_CLK, DS1302_DAT, DS1302_RST);
    return 0;
}

static void __exit ds1302_exit(void)
{
    gpio_set_value(DS1302_RST, 0);

    gpio_free(DS1302_CLK);
    gpio_free(DS1302_DAT);
    gpio_free(DS1302_RST);

    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
}

module_init(ds1302_init);
module_exit(ds1302_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("User");

