#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>

#define DRIVER_NAME "rotary_device_driver"
#define S1_GPIO 5 
#define S2_GPIO 6 
#define SW_GPIO 13 
#define DEBOUNCE_MS 150 
#define ROTARY_DEBOUNCE_MS 10 

static dev_t device_number;
static struct cdev rotary_cdev;
static struct class *rotary_class;
static int interrupt_num_s1, interrupt_num_sw;
static long rotary_value = 0;
static int button_status = 1; // 1: 뗌, 0: 누름 (Active Low)
static unsigned long last_rot_jiffies = 0, last_sw_jiffies = 0;
static int data_ready = 0;
static DECLARE_WAIT_QUEUE_HEAD(rotary_wait_queue);

static unsigned int rotary_poll(struct file *file, poll_table *wait)
{
    poll_wait(file, &rotary_wait_queue, wait);
    if (data_ready)
        return POLLIN | POLLRDNORM;
    return 0;
}

static irqreturn_t rotary_sw_handler(int irq, void *dev_id) {
    if (time_before(jiffies, last_sw_jiffies + msecs_to_jiffies(DEBOUNCE_MS))) return IRQ_HANDLED;
    last_sw_jiffies = jiffies;

    // 현재 버튼의 물리적 상태(0 또는 1)를 직접 읽음
    button_status = gpio_get_value(SW_GPIO); 
    data_ready = 1;
    wake_up_interruptible(&rotary_wait_queue);
    return IRQ_HANDLED;
}

static irqreturn_t rotary_int_handler(int irq, void *dev_id) {
    if (time_before(jiffies, last_rot_jiffies + msecs_to_jiffies(ROTARY_DEBOUNCE_MS))) return IRQ_HANDLED;
    last_rot_jiffies = jiffies;

    if (gpio_get_value(S1_GPIO) == 0) {
        if (gpio_get_value(S2_GPIO) == 1) rotary_value--; 
        else                             rotary_value++;
    }
    data_ready = 1;
    wake_up_interruptible(&rotary_wait_queue);
    return IRQ_HANDLED;
}

static ssize_t rotary_read(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {
    char buff[64];

    if (!data_ready) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        wait_event_interruptible(rotary_wait_queue, data_ready != 0);
    }

    int len = snprintf(buff, sizeof(buff), "%ld %d\n", rotary_value, button_status);
    data_ready = 0;

    if (copy_to_user(user_buf, buff, len)) return -EFAULT;
    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = rotary_read,
    .poll  = rotary_poll,
};

static int __init rotary_init(void) {
    alloc_chrdev_region(&device_number, 0, 1, DRIVER_NAME);
    cdev_init(&rotary_cdev, &fops);
    cdev_add(&rotary_cdev, device_number, 1);
    rotary_class = class_create(THIS_MODULE, DRIVER_NAME);
    device_create(rotary_class, NULL, device_number, NULL, DRIVER_NAME);

    gpio_request(S1_GPIO, "s1"); gpio_direction_input(S1_GPIO);
    gpio_request(S2_GPIO, "s2"); gpio_direction_input(S2_GPIO);
    gpio_request(SW_GPIO, "sw"); gpio_direction_input(SW_GPIO);

    interrupt_num_s1 = gpio_to_irq(S1_GPIO);
    request_irq(interrupt_num_s1, rotary_int_handler, IRQF_TRIGGER_FALLING, "rot_irq_s1", NULL);

    interrupt_num_sw = gpio_to_irq(SW_GPIO);
    // [핵심 수정] RISING과 FALLING을 모두 감지하여 누름/뗌 체크 가능하게 함
    request_irq(interrupt_num_sw, rotary_sw_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "rot_irq_sw", NULL);

    return 0;
}

static void __exit rotary_exit(void) {
    free_irq(interrupt_num_s1, NULL); free_irq(interrupt_num_sw, NULL);
    gpio_free(S1_GPIO); gpio_free(S2_GPIO); gpio_free(SW_GPIO);
    device_destroy(rotary_class, device_number); class_destroy(rotary_class);
    cdev_del(&rotary_cdev); unregister_chrdev_region(device_number, 1);
}

module_init(rotary_init); module_exit(rotary_exit);
MODULE_LICENSE("GPL");
