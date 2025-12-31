#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>

#define DRIVER_NAME "ssd1306_driver"
#define CLASS_NAME  "ssd1306_class"

#define SSD1306_I2C_ADDR   0x3C
#define MAX_BUFFER_SIZE   1024

/* SSD1306 Commands */
#define SSD1306_DISPLAYOFF          0xAE
#define SSD1306_DISPLAYON           0xAF
#define SSD1306_SETDISPLAYCLOCKDIV  0xD5
#define SSD1306_SETMULTIPLEX        0xA8
#define SSD1306_SETDISPLAYOFFSET    0xD3
#define SSD1306_SETSTARTLINE        0x40
#define SSD1306_CHARGEPUMP          0x8D
#define SSD1306_MEMORYMODE          0x20
#define SSD1306_SEGREMAP            0xA1
#define SSD1306_COMSCANDEC          0xC8
#define SSD1306_SETCOMPINS          0xDA
#define SSD1306_SETCONTRAST         0x81
#define SSD1306_SETPRECHARGE        0xD9
#define SSD1306_SETVCOMDETECT       0xDB
#define SSD1306_DISPLAYALLON_RESUME 0xA4
#define SSD1306_NORMALDISPLAY       0xA6

struct ssd1306_dev {
    struct i2c_client *client;
    struct cdev cdev;
    struct class *class;
    dev_t dev_num;
};

static struct ssd1306_dev *ssd1306_device;

/* ================= I2C Write ================= */

/* Control Byte 0x00 = Command */
static int ssd1306_write_cmd(struct ssd1306_dev *dev, u8 cmd)
{
    u8 buf[2] = { 0x00, cmd };
    return i2c_master_send(dev->client, buf, 2);
}

/* Control Byte 0x40 = Data */
static int ssd1306_write_data(struct ssd1306_dev *dev, u8 *data, size_t len)
{
    u8 *buf;
    int ret;

    buf = kmalloc(len + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    buf[0] = 0x40;
    memcpy(&buf[1], data, len);

    ret = i2c_master_send(dev->client, buf, len + 1);
    kfree(buf);

    return ret;
}

/* ================= Init Sequence ================= */

static void ssd1306_init_seq(struct ssd1306_dev *dev)
{
    ssd1306_write_cmd(dev, SSD1306_DISPLAYOFF);

    ssd1306_write_cmd(dev, SSD1306_SETDISPLAYCLOCKDIV);
    ssd1306_write_cmd(dev, 0x80);

    ssd1306_write_cmd(dev, SSD1306_SETMULTIPLEX);
    ssd1306_write_cmd(dev, 0x3F);

    ssd1306_write_cmd(dev, SSD1306_SETDISPLAYOFFSET);
    ssd1306_write_cmd(dev, 0x00);

    ssd1306_write_cmd(dev, SSD1306_SETSTARTLINE | 0x00);

    ssd1306_write_cmd(dev, SSD1306_CHARGEPUMP);
    ssd1306_write_cmd(dev, 0x14);

    ssd1306_write_cmd(dev, SSD1306_MEMORYMODE);
    ssd1306_write_cmd(dev, 0x00);

    ssd1306_write_cmd(dev, SSD1306_SEGREMAP);
    ssd1306_write_cmd(dev, SSD1306_COMSCANDEC);

    ssd1306_write_cmd(dev, SSD1306_SETCOMPINS);
    ssd1306_write_cmd(dev, 0x12);

    ssd1306_write_cmd(dev, SSD1306_SETCONTRAST);
    ssd1306_write_cmd(dev, 0xCF);

    ssd1306_write_cmd(dev, SSD1306_SETPRECHARGE);
    ssd1306_write_cmd(dev, 0xF1);

    ssd1306_write_cmd(dev, SSD1306_SETVCOMDETECT);
    ssd1306_write_cmd(dev, 0x40);

    ssd1306_write_cmd(dev, SSD1306_DISPLAYALLON_RESUME);
    ssd1306_write_cmd(dev, SSD1306_NORMALDISPLAY);
    ssd1306_write_cmd(dev, SSD1306_DISPLAYON);
}

/* ================= File Operations ================= */

static int ssd1306_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int ssd1306_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t ssd1306_write(struct file *file,
                             const char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    u8 *kbuf;

    if (count > MAX_BUFFER_SIZE)
        count = MAX_BUFFER_SIZE;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    ssd1306_write_data(ssd1306_device, kbuf, count);
    kfree(kbuf);

    return count;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = ssd1306_open,
    .release = ssd1306_release,
    .write   = ssd1306_write,
};

/* ================= I2C Probe ================= */

static int ssd1306_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct ssd1306_dev *dev;

    dev_info(&client->dev, "SSD1306 I2C OLED Probed\n");

    dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->client = client;
    ssd1306_device = dev;
    i2c_set_clientdata(client, dev);

    /* char device */
    alloc_chrdev_region(&dev->dev_num, 0, 1, DRIVER_NAME);
    dev->class = class_create(THIS_MODULE, CLASS_NAME);

    cdev_init(&dev->cdev, &fops);
    cdev_add(&dev->cdev, dev->dev_num, 1);

    device_create(dev->class, NULL, dev->dev_num, NULL, DRIVER_NAME);

    /* OLED init */
    ssd1306_init_seq(dev);

    dev_info(&client->dev, "SSD1306 Initialized (I2C, 4-pin)\n");
    return 0;
}

static void ssd1306_remove(struct i2c_client *client)
{
    struct ssd1306_dev *dev = i2c_get_clientdata(client);

    ssd1306_write_cmd(dev, SSD1306_DISPLAYOFF);

    device_destroy(dev->class, dev->dev_num);
    cdev_del(&dev->cdev);
    class_destroy(dev->class);
    unregister_chrdev_region(dev->dev_num, 1);
}

/* ================= I2C Driver ================= */

static const struct i2c_device_id ssd1306_id[] = {
    { "ssd1306", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ssd1306_id);

static struct i2c_driver ssd1306_driver = {
    .driver = {
        .name = DRIVER_NAME,
    },
    .probe    = ssd1306_probe,
    .remove   = ssd1306_remove,
    .id_table = ssd1306_id,
};

module_i2c_driver(ssd1306_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("SSD1306 I2C OLED Driver (4-pin)");


