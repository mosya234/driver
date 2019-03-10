/* qma6981 motion sensor driver
 *
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
 
#include <accel.h>
#include <cust_acc.h>
#include "qma6981.h"

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/gpio.h>

 //step counter
#include <asm/io.h>
#include <linux/sched.h>

//#define QMA6981_WRITE_OFFSET_TO_REG
//#define QMA6981_MTK_I2C_FUNCTION

#define QMA6981_RETRY_COUNT 3

#define qma6981_BUFSIZE		256

#define GSE_TAG                  "[QMA-Gsensor] "
#define GSE_ERR(fmt, args...)    pr_err(GSE_TAG "%s %d : " fmt, __FUNCTION__, __LINE__, ##args)
#define GSE_FUN(f)               pr_debug(GSE_TAG "%s\n", __FUNCTION__)
//#define GSE_DEBUG
#ifdef GSE_DEBUG
#define GSE_LOG(fmt, args...)    pr_debug(GSE_TAG "%s %d : " fmt, __FUNCTION__, __LINE__, ##args)
#else
#define GSE_LOG(fmt, args...)    
#endif

#define kal_bool	bool
#define KAL_TRUE	true
#define KAL_FALSE	false

#define GSENSOR_IOCTL_SET_STICK_CHECK_REG         	_IOW(GSENSOR, 0x11, int)
#define GSENSOR_IOCTL_GET_STICK_STATUS         		_IOR(GSENSOR, 0x12, int)

static struct acc_hw accel_cust;
static struct acc_hw *hw = &accel_cust;
static int qma6981_init_flag =-1;
static DEFINE_MUTEX(qma6981_init_mutex);
static DEFINE_MUTEX(qma6981_mutex);


#if defined(CONFIG_HAS_EARLYSUSPEND)
static void qma6981_early_suspend(struct early_suspend *h);
static void qma6981_late_resume(struct early_suspend *h);
#endif
static int qma6981_local_init(void);
static int qma6981_local_uninit(void);

static struct acc_init_info qma6981_init_info = {
	.name = QMA6981_ACC_DEV_NAME,
	.init = qma6981_local_init,
	.uninit = qma6981_local_uninit,	
};

struct qma6981_data{
	struct 	acc_hw *hw;
	struct 	i2c_client *client;
	struct 	hwmsen_convert   cvt;	
	int 					 sensitivity;
    atomic_t   				 layout;
	atomic_t                 trace;
	atomic_t                 suspend;

	s16                      cali_sw[QMA6981_AXES_NUM+1];
	/*data*/
	s8                       offset[QMA6981_AXES_NUM+1];  /*+1: for 4-byte alignment*/
	s16                      data[QMA6981_AXES_NUM+1];
	u8                       bandwidth;
	
	/*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND)
	struct early_suspend    early_drv;
#endif 
};

static struct qma6981_data *qma6981 = NULL;
struct i2c_client *qma6981_i2c_client = NULL;
static bool sensor_power = true;
static bool enable_status = false;
int QMA6981_Chip_id = 0;
//static unsigned long long int_top_time = 0;
static u8 qm6981_248g_offset = 8;
// add by yangzhiqiang for cic
#define QMA6981_CIC_THRESHOLD		6000
static int qma6981_stick_status = 0;
static short cic_z_00, cic_z_1f;
static void qma6981_set_stick_check_reg(int flag);
// yangzhiqiang

static int I2C_RxData(char *rxData, int length)
{	
	uint8_t loop_i;    
	int res = 0;
	//struct qma6981_data *obj = (struct qma6981_data*)i2c_get_clientdata(qma6981_i2c_client);

	//if (atomic_read(&obj->suspend))
	//{
	//	printk("FCH Gsensor in suspend can not read");
	//	return -1;
	//}

	mutex_lock(&qma6981_mutex);
	if((rxData == NULL) || (length < 1))	
	{	
		GSE_ERR("qma6981 I2C_RxData error");
		mutex_unlock(&qma6981_mutex);
		return -EINVAL;	
	}	

	for(loop_i = 0; loop_i < QMA6981_RETRY_COUNT; loop_i++)	
	{		
		qma6981_i2c_client->addr = (qma6981_i2c_client->addr & I2C_MASK_FLAG) | I2C_WR_FLAG;		
		res = i2c_master_send(qma6981_i2c_client, (const char*)rxData, ((length<<0X08) | 0X01));		
		if(res > 0)
			break;

		GSE_ERR("qma6981 i2c_read retry %d times\n", loop_i);
		mdelay(10);	
	}    	
	qma6981_i2c_client->addr = qma6981_i2c_client->addr & I2C_MASK_FLAG;

	if(loop_i >= QMA6981_RETRY_COUNT)	
	{		
		GSE_ERR("qma6981 %s retry over %d\n", __func__, QMA6981_RETRY_COUNT);
		mutex_unlock(&qma6981_mutex);
		return -EIO;	
	}
		mutex_unlock(&qma6981_mutex);
	return 0;
}

static int I2C_TxData(char *txData, int length)
{
	uint8_t loop_i;

	mutex_lock(&qma6981_mutex);
	/* Caller should check parameter validity.*/
	if ((txData == NULL) || (length < 2))
	{
		mutex_unlock(&qma6981_mutex);
		return -EINVAL;
	}

	qma6981_i2c_client->addr = qma6981_i2c_client->addr & I2C_MASK_FLAG;
	for(loop_i = 0; loop_i < QMA6981_RETRY_COUNT; loop_i++)
	{
		if(i2c_master_send(qma6981_i2c_client, (const char*)txData, length) > 0)
			break;

		GSE_ERR("I2C_TxData delay!\n");
		mdelay(10);
	}

	if(loop_i >= QMA6981_RETRY_COUNT)
	{
		GSE_ERR( "%s retry over %d\n", __func__, QMA6981_RETRY_COUNT);	
		mutex_unlock(&qma6981_mutex);
		return -EIO;
	}
	mutex_unlock(&qma6981_mutex);
	return 0;
}

/* X,Y and Z-axis accelerometer data readout
*/
static int qma6981_read_raw_xyz(int *data){
	//int res;	
	unsigned char databuf[6] = {0};		
	unsigned char i;	

	databuf[0] = QMA6981_XOUTL;		
	if(0 != I2C_RxData(databuf, 6)){
		GSE_ERR("read xyz error!!!");
		return -EFAULT;	
	}	

 	data[0]  = (s16)((databuf[1]<<2) |( databuf[0]>>6));
	data[1]  = (s16)((databuf[3]<<2) |( databuf[2]>>6));
	data[2]  = (s16)((databuf[5]<<2) |( databuf[4]>>6));

#if 1
	for(i=0;i<3;i++)				
	{	//because the data is store in binary complement number formation in computer system
		if ( data[i] == 0x0200 )	//so we want to calculate actual number here
			data[i]= -512;		//10bit resolution, 512= 2^(10-1)
		else if ( data[i] & 0x0200 )	//transfor format
		{					//printk("data 0 step %x \n",data[i]);
			data[i] -= 0x1;			//printk("data 1 step %x \n",data[i]);
			data[i] = ~data[i];		//printk("data 2 step %x \n",data[i]);
			data[i] &= 0x01ff;		//printk("data 3 step %x \n\n",data[i]);
			data[i] = -data[i];		
		}
	}
	//GSE_LOG("qma6981 raw LBS data  %d,%d,%d\n",data[0],data[1],data[2]);
#endif
	//printk("yzqaccraw	%d	%d	%d\n", data[0], data[1], data[2]);

	return 0;
}

static int qma6981_read_acc_xyz(int *data){
	
	int raw[3]={0};
	int acc[3]={0};
	int ret = 0;

	ret = qma6981_read_raw_xyz(raw);
	if(0 != ret ){
		GSE_ERR("qma6981_read_acc_xyz error\n");
	}
	
	//GSE_LOG("qma6981 AFTER x4:%d,y:%d,z:%d\n",raw[0],raw[1],raw[2]);
#if defined(QMA6981_WRITE_OFFSET_TO_REG)
#else
	raw[QMA6981_AXIS_X] += qma6981->cali_sw[QMA6981_AXIS_X];
	raw[QMA6981_AXIS_Y] += qma6981->cali_sw[QMA6981_AXIS_Y];
	raw[QMA6981_AXIS_Z] += qma6981->cali_sw[QMA6981_AXIS_Z];
#endif
	
	//remap coordinate
	acc[qma6981->cvt.map[QMA6981_AXIS_X]] = qma6981->cvt.sign[QMA6981_AXIS_X]*raw[QMA6981_AXIS_X];
	acc[qma6981->cvt.map[QMA6981_AXIS_Y]] = qma6981->cvt.sign[QMA6981_AXIS_Y]*raw[QMA6981_AXIS_Y];
	acc[qma6981->cvt.map[QMA6981_AXIS_Z]] = qma6981->cvt.sign[QMA6981_AXIS_Z]*raw[QMA6981_AXIS_Z];
	//GSE_LOG("qma6981 AFTER x1:%d,y:%d,z:%d\n",data[0],data[1],data[2]);

	data[QMA6981_AXIS_X] = (acc[QMA6981_AXIS_X]*GRAVITY_EARTH_1000)>>(qm6981_248g_offset);
	data[QMA6981_AXIS_Y] = (acc[QMA6981_AXIS_Y]*GRAVITY_EARTH_1000)>>(qm6981_248g_offset);
	data[QMA6981_AXIS_Z] = (acc[QMA6981_AXIS_Z]*GRAVITY_EARTH_1000)>>(qm6981_248g_offset);

	//printk("yzqacc	%d	%d	%d\n", data[0], data[1], data[2]);

	return ret;
}

// add by yangzhiqiang
#if defined(QMA6981_STORE_CALI)
static void qma6981_write_file(char * filename, char *data, int len)
{
	struct file *fp;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(KERNEL_DS);
	fp = filp_open(filename, O_RDWR|O_CREAT, 0666);
	if (IS_ERR(fp))
	{
		printk("qma6981_write_file open file error\n");
	}
	else
	{
		printk("qma6981_write_file data=0x%x len=%d\n", data, len);
		//snprintf();
		fp->f_op->write(fp, data, len , &fp->f_pos);
		filp_close(fp, NULL);
	}

	set_fs(fs);
}

static void qma6981_read_file(char * filename, char *data, int len)
{
	struct file *fp;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(KERNEL_DS);
	fp = filp_open(filename, O_RDONLY, 0666);
	if (IS_ERR(fp))
	{
		printk("qma6981_read_file open file error\n");
	}
	else
	{
		printk("qma6981_read_file data=0x%x len=%d\n", data, len);
		fp->f_op->read(fp, data, len , &fp->f_pos);
		filp_close(fp, NULL);
	}

	set_fs(fs);
}
#endif
// yangzhiqiang

/*----------------------------------------------------------------------------*/
static int qma6981_ReadOffset(struct i2c_client *client, s8 ofs[QMA6981_AXES_NUM])
{    
	int err = 0;

	ofs[1]=ofs[2]=ofs[0]=0x00;

	//GSE_LOG("qma6981 offesx=%x, y=%x, z=%x\n",ofs[0],ofs[1],ofs[2]);
	
	return err;    
}

#if defined(QMA6981_WRITE_OFFSET_TO_REG)
static int qma6981_WriteOffset(struct i2c_client *client, s8 x_offset, s8 y_offset, s8 z_offset)
{
	int ret;
	char data[2];

	printk("qma6981_WriteOffset %d %d %d\n", x_offset, y_offset, z_offset);
	data[0] = 0x27;
	data[1] = x_offset;
	ret = I2C_TxData(data,2);

	data[0] = 0x28;
	data[1] = y_offset;
	ret = I2C_TxData(data,2);

	data[0] = 0x29;
	data[1] = z_offset;
	ret = I2C_TxData(data,2);

	return 0;
}
#endif

/*----------------------------------------------------------------------------*/
static int qma6981_ResetCalibration(struct i2c_client *client)
{
	struct qma6981_data *obj = i2c_get_clientdata(client);
	int err = 0;
	//GSE_LOG("qma6981 qma6981_ResetCalibration\n");
	memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
	memset(obj->offset, 0x00, sizeof(obj->offset));
#if defined(QMA6981_WRITE_OFFSET_TO_REG)
	qma6981_WriteOffset(client, obj->cali_sw[0], obj->cali_sw[1], obj->cali_sw[2]);
#endif
	return err;    
}

/*----------------------------------------------------------------------------*/
static int qma6981_ReadCalibrationEx(struct i2c_client *client, int act[QMA6981_AXES_NUM], int raw[QMA6981_AXES_NUM])
{  
	/*raw: the raw calibration data; act: the actual calibration data*/
	struct qma6981_data *obj = i2c_get_clientdata(client);
	int mul;

	mul = 0;//only SW Calibration, disable HW Calibration
	//GSE_LOG("qma6981 qma6981_ReadCalibrationEx\n");
	raw[QMA6981_AXIS_X] = obj->offset[QMA6981_AXIS_X]*mul + obj->cali_sw[QMA6981_AXIS_X];
	raw[QMA6981_AXIS_Y] = obj->offset[QMA6981_AXIS_Y]*mul + obj->cali_sw[QMA6981_AXIS_Y];
	raw[QMA6981_AXIS_Z] = obj->offset[QMA6981_AXIS_Z]*mul + obj->cali_sw[QMA6981_AXIS_Z];

	act[obj->cvt.map[QMA6981_AXIS_X]] = obj->cvt.sign[QMA6981_AXIS_X]*raw[QMA6981_AXIS_X];
	act[obj->cvt.map[QMA6981_AXIS_Y]] = obj->cvt.sign[QMA6981_AXIS_Y]*raw[QMA6981_AXIS_Y];
	act[obj->cvt.map[QMA6981_AXIS_Z]] = obj->cvt.sign[QMA6981_AXIS_Z]*raw[QMA6981_AXIS_Z];                        
	                       
	return 0;
}

/*----------------------------------------------------------------------------*/
static int qma6981_WriteCalibration(struct i2c_client *client, int dat[QMA6981_AXES_NUM])
{
	struct qma6981_data *obj = i2c_get_clientdata(client);
	int err = 0;
	int cali[QMA6981_AXES_NUM], raw[QMA6981_AXES_NUM];

	if(0 != qma6981_ReadCalibrationEx(client, cali, raw))	/*offset will be updated in obj->offset*/
	{ 
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}

		/*calculate the real offset expected by caller*/
	cali[QMA6981_AXIS_X] += dat[QMA6981_AXIS_X];
	cali[QMA6981_AXIS_Y] += dat[QMA6981_AXIS_Y];
	cali[QMA6981_AXIS_Z] += dat[QMA6981_AXIS_Z];

	//GSE_LOG("UPDATE: (%+3d %+3d %+3d)\n", 
	//	dat[QMA6981_AXIS_X], dat[QMA6981_AXIS_Y], dat[QMA6981_AXIS_Z]);

	obj->cali_sw[QMA6981_AXIS_X] = obj->cvt.sign[QMA6981_AXIS_X]*(cali[obj->cvt.map[QMA6981_AXIS_X]]);
	obj->cali_sw[QMA6981_AXIS_Y] = obj->cvt.sign[QMA6981_AXIS_Y]*(cali[obj->cvt.map[QMA6981_AXIS_Y]]);
	obj->cali_sw[QMA6981_AXIS_Z] = obj->cvt.sign[QMA6981_AXIS_Z]*(cali[obj->cvt.map[QMA6981_AXIS_Z]]);	
#if defined(QMA6981_WRITE_OFFSET_TO_REG)	//defined(QMA6981_STORE_CALI)
	qma6981_WriteOffset(client, obj->cali_sw[0], obj->cali_sw[1], obj->cali_sw[2]);
	//printk("qma6981 qma6981_WriteCalibration dat= %d %d %d\n", obj->cali_sw[0],obj->cali_sw[1],obj->cali_sw[2]);
	//qma6981_write_file("/data/misc/sensor/qma6981_cali.txt", (char *)(obj->cali_sw), sizeof(obj->cali_sw));
#endif

	return err;
}
/*----------------------------------------------------------------------------*/
static int qma6981_ReadCalibration(struct i2c_client *client, int dat[QMA6981_AXES_NUM])
{
    struct qma6981_data *obj = i2c_get_clientdata(client);
    int mul;
	
	mul = 0;//only SW Calibration, disable HW Calibration
	
    dat[obj->cvt.map[QMA6981_AXIS_X]] = obj->cvt.sign[QMA6981_AXIS_X]*(obj->offset[QMA6981_AXIS_X]*mul + obj->cali_sw[QMA6981_AXIS_X]);
    dat[obj->cvt.map[QMA6981_AXIS_Y]] = obj->cvt.sign[QMA6981_AXIS_Y]*(obj->offset[QMA6981_AXIS_Y]*mul + obj->cali_sw[QMA6981_AXIS_Y]);
    dat[obj->cvt.map[QMA6981_AXIS_Z]] = obj->cvt.sign[QMA6981_AXIS_Z]*(obj->offset[QMA6981_AXIS_Z]*mul + obj->cali_sw[QMA6981_AXIS_Z]);                    
#if defined(QMA6981_STORE_CALI)
	//qma6981_read_file("/data/misc/sensor/qma6981_cali.txt", (char *)(cali_sw), sizeof(cali_sw));
	//printk("qma6981 cali_sw %d %d %d\n", cali_sw[0],cali_sw[1],cali_sw[2]);
#endif
    return 0;

	
}/*-----------------------------------------------------------------------------*/


static ssize_t show_dumpallreg_value(struct device_driver *ddri, char *buf)
{
	int res;
	int i =0;
	char strbuf[1024];
	char tempstrbuf[24];
	unsigned char databuf[2];
	int length=0;

	GSE_FUN();

	/* Check status register for data availability */
	for(i =0;i<64;i++)
	{
		databuf[0] = i;
		res = I2C_RxData(databuf, 1);
		if(res < 0)
			GSE_LOG("qma6981 dump registers 0x%02x failed !\n", i);

		length = scnprintf(tempstrbuf, sizeof(tempstrbuf), "reg[0x%2x] =  0x%2x \n",i, databuf[0]);
		snprintf(strbuf+length*i, sizeof(strbuf)-length*i, "%s \n",tempstrbuf);
	}

	return scnprintf(buf, sizeof(strbuf), "%s\n", strbuf);
}


/*----------------------------------------------------------------------------*/
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	char strbuf[qma6981_BUFSIZE];
	int output;
	unsigned char databuf;


	databuf = QMA6981_CHIP_ID;
	if(0 != I2C_RxData(&databuf, 1))
	{
		GSE_LOG("read chip id error!!!");
		return -EFAULT;
	}
	output = (int)databuf;

	sprintf(strbuf, "chipid:%d \n", output);

	return sprintf(buf, "%s\n", strbuf);
}

static ssize_t show_waferid_value(struct device_driver *ddri, char *buf)
{
	int res;
	
	unsigned int chipid;
	unsigned char chipidh;
	unsigned char chipidl;
	
	unsigned char waferid;
	unsigned char waferid1;
	unsigned char waferid2;
	unsigned char waferid3;

	
	chipidh = 0x48;
	if((res = I2C_RxData(&chipidh, 1)))
	{
		GSE_LOG("read wafer chip h error!!!\n");
		return -EFAULT;
	}
	chipidl = 0x47;
	if((res = I2C_RxData(&chipidl, 1)))
	{
		GSE_LOG("read wafer chip l error!!!\n");
		return -EFAULT;
	}
	GSE_LOG("read wafer chip H:0x%x L:0x%x", chipidh, chipidl);
	chipid = (chipidh<<8)|chipidl;
	
	waferid1 = 0x59;
	if((res = I2C_RxData(&waferid1, 1)))
	{
		GSE_LOG("read wafer id 1 error!!!\n");
		return -EFAULT;
	}
	waferid2 = 0x41;
	if((res = I2C_RxData(&waferid2, 1)))
	{
		GSE_LOG("read wafer id 2 error!!!\n");
		return -EFAULT;
	}
	waferid3 = 0x40;
	if((res = I2C_RxData(&waferid3, 1)))
	{
		GSE_LOG("read wafer id 3 error!!!\n");
		return -EFAULT;
	}
	
	GSE_LOG("wafer ID: 0x%x 0x%x 0x%x\n", waferid1, waferid2, waferid3);
	
	waferid = (waferid1&0x10)|((waferid2>>4)&0x0c)|((waferid3>>6)&0x03);

	return sprintf(buf, " Chip id:0x%x \n Wafer ID 0x%02x\n", chipid, waferid);
}


/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	ssize_t res;
	struct qma6981_data *obj = i2c_get_clientdata(qma6981_i2c_client);
	if(NULL == obj)
	{
		GSE_LOG("qma6981_data is null!!\n");
		return 0;
	}

	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));
	return res;
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct qma6981_data *obj = i2c_get_clientdata(qma6981_i2c_client);
	int trace;
	if(NULL == obj)
	{
		GSE_LOG("qma6981_data is null!!\n");
		return 0;
	}

	if(1 == sscanf(buf, "0x%x", &trace))
	{
		atomic_set(&obj->trace, trace);
	}
	else
	{
		GSE_LOG("invalid content: '%s', length = %d\n", buf, (int)count);
	}

	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t show_cali_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = qma6981_i2c_client;
	struct qma6981_data *obj;
	int err, len = 0, mul;
	int tmp[QMA6981_AXES_NUM];

	if(NULL == client)
	{
		GSE_LOG("i2c client is null!!\n");
		return 0;
	}

	obj = i2c_get_clientdata(client);
	
	err = qma6981_ReadOffset(client, obj->offset);
	if(0 != err)
	{
		return -EINVAL;
	}
	else if(0 != (err = qma6981_ReadCalibration(client, tmp)))
	{
		return -EINVAL;
	}
	else
	{    
		mul = 256/64;
		len += snprintf(buf+len, PAGE_SIZE-len, "[HW ][%d] (%+3d, %+3d, %+3d) : (0x%02X, 0x%02X, 0x%02X)\n", mul,                        
			obj->offset[QMA6981_AXIS_X], obj->offset[QMA6981_AXIS_Y], obj->offset[QMA6981_AXIS_Z],
			obj->offset[QMA6981_AXIS_X], obj->offset[QMA6981_AXIS_Y], obj->offset[QMA6981_AXIS_Z]);
		len += snprintf(buf+len, PAGE_SIZE-len, "[SW ][%d] (%+3d, %+3d, %+3d)\n", 1, 
			obj->cali_sw[QMA6981_AXIS_X], obj->cali_sw[QMA6981_AXIS_Y], obj->cali_sw[QMA6981_AXIS_Z]);

		len += snprintf(buf+len, PAGE_SIZE-len, "[ALL]    (%+3d, %+3d, %+3d) : (%+3d, %+3d, %+3d)\n", 
			obj->offset[QMA6981_AXIS_X]*mul + obj->cali_sw[QMA6981_AXIS_X],
			obj->offset[QMA6981_AXIS_Y]*mul + obj->cali_sw[QMA6981_AXIS_Y],
			obj->offset[QMA6981_AXIS_Z]*mul + obj->cali_sw[QMA6981_AXIS_Z],
			tmp[QMA6981_AXIS_X], tmp[QMA6981_AXIS_Y], tmp[QMA6981_AXIS_Z]);
		
		return len;
    }
}
/*----------------------------------------------------------------------------*/
static ssize_t store_cali_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct i2c_client *client = qma6981_i2c_client;  
	int err, x, y, z;
	int dat[QMA6981_AXES_NUM];

	if(!strncmp(buf, "rst", 3))
	{
		if(0 != (err = qma6981_ResetCalibration(client)))
		{
			GSE_LOG("reset offset err = %d\n", err);
		}	
	}
	else if(3 == sscanf(buf, "0x%02X,0x%02X,0x%02X", &x, &y, &z))
	{
		dat[QMA6981_AXIS_X] = x;
		dat[QMA6981_AXIS_Y] = y;
		dat[QMA6981_AXIS_Z] = z;
		if(0 != (err = qma6981_WriteCalibration(client, dat)))
		{
			GSE_LOG("write calibration err = %d\n", err);
		}		
	}
	else
	{
		GSE_LOG("invalid format\n");
	}
	
	return count;
}

/*----------------------------------------------------------------------------*/
static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{

	int sensordata[3];
	char strbuf[qma6981_BUFSIZE];

	qma6981_read_acc_xyz(sensordata);
	//qma6981_read_acc_xyz((int*)&sensordata); //cxg 0209

	sprintf(strbuf, "%d %d %d\n", sensordata[0],sensordata[1],sensordata[2]);

	return sprintf(buf, "%s\n", strbuf);
}



/*----------------------------------------------------------------------------*/
static ssize_t show_layout_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = qma6981_i2c_client;
	struct qma6981_data *data = i2c_get_clientdata(client);
	int err;
	
	data->hw = hw;
	
	if(0 != (err = hwmsen_get_convert(data->hw->direction, &data->cvt)))
	{
		GSE_ERR("invalid direction: %d\n", data->hw->direction);
	}
	
	return scnprintf(buf, PAGE_SIZE, "(%d, %d)\n[%+2d %+2d %+2d]\n[%+2d %+2d %+2d]\n",
		data->hw->direction,atomic_read(&data->layout),data->cvt.sign[0], data->cvt.sign[1],
		data->cvt.sign[2],data->cvt.map[0], data->cvt.map[1], data->cvt.map[2]);
}
/*----------------------------------------------------------------------------*/
static ssize_t store_layout_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct i2c_client *client = qma6981_i2c_client;
    struct qma6981_data *data = i2c_get_clientdata(client);

	int layout = 0;

	if(1 == sscanf(buf, "%d", &layout))
	{
		atomic_set(&data->layout, layout);
		if(!hwmsen_get_convert(layout, &data->cvt))
		{
			GSE_ERR("HWMSEN_GET_CONVERT function error!\r\n");
		}
		else if(!hwmsen_get_convert(data->hw->direction, &data->cvt))
		{
			GSE_ERR("invalid layout: %d, restore to %d\n", layout, data->hw->direction);
		}
		else
		{
			GSE_ERR("invalid layout: (%d, %d)\n", layout, data->hw->direction);
			hwmsen_get_convert(0, &data->cvt);
		}
	}
	else
	{
		GSE_ERR("invalid format = '%s'\n", buf);
	}
	return count;
}

// add by yangzhiqiang for debug reg use adb
static u8 qma6981_debug_reg_addr=0x00;
static u8 qma6981_debug_read_len=0x01;

static ssize_t store_setreg(struct device_driver *ddri, const char *buf, size_t count)
{
	unsigned int addr, value;
	u8 data[C_I2C_FIFO_SIZE];
	int res = 0;
	
	GSE_LOG("store_setreg buf=%s count=%d\n", buf, (int)count);
	if(2 == sscanf(buf, "0x%x 0x%x", &addr, &value))
	{
		GSE_LOG("get para OK addr=0x%x value=0x%x\n", addr, value);
		qma6981_debug_reg_addr = (u8)addr;
		qma6981_debug_read_len = 1;
		data[0] = (u8)addr;
		data[1] = (u8)value;
		res = I2C_TxData(data,2);
		if(res)
		{
			GSE_ERR("write reg 0x%02x fail\n", addr);
		}
	}
	else
	{
		GSE_ERR("store_reg get para error\n");
	}

	return count;
}

static ssize_t show_getreg(struct device_driver *ddri, char *buf)
{
	u8 data[C_I2C_FIFO_SIZE];
	int write_offset=0, icount=0;
	int res = 0;

	if(qma6981->client)
	{
		memset(data, 0, sizeof(data));

		data[0] = qma6981_debug_reg_addr;
		res = I2C_RxData(data, qma6981_debug_read_len);
		if(res)
		{
			GSE_ERR("read reg 0x%02x fail", qma6981_debug_reg_addr);
		}
		write_offset += snprintf(buf+write_offset, PAGE_SIZE-write_offset, "0x%02x=", qma6981_debug_reg_addr);
		for(icount=0; icount<qma6981_debug_read_len; icount++)
		{
			write_offset += snprintf(buf+write_offset, PAGE_SIZE-write_offset, "0x%02x ", data[icount]);
		}
		write_offset += snprintf(buf+write_offset, PAGE_SIZE-write_offset, "\n");

		return write_offset;
	}
	else
	{
		return snprintf(buf, PAGE_SIZE, "error!");
	}
}

static ssize_t store_getreg(struct device_driver *ddri, const char *buf, size_t count)
{
	unsigned int addr, value;
	
	
	//GSE_LOG("store_getreg buf=%s count=%d\n", buf, count);
	if(2 == sscanf(buf, "0x%x %d", &addr, &value))
	{
		GSE_LOG("get para OK addr=0x%x len=%d\n", qma6981_debug_reg_addr, qma6981_debug_read_len);
		qma6981_debug_reg_addr = (u8)addr;
		qma6981_debug_read_len = (u8)value;
		if(qma6981_debug_read_len >= C_I2C_FIFO_SIZE)
		{
			qma6981_debug_read_len = C_I2C_FIFO_SIZE - 1;
			GSE_ERR("len must < %d\n", C_I2C_FIFO_SIZE);
		}
	}
	else
	{
		GSE_ERR("store_reg get para error\n");
	}
	return count;
}
// add by yangzhiqiang for debug

static ssize_t show_cic(struct device_driver *ddri, char *buf)
{
	qma6981_set_stick_check_reg(1);
	mdelay(5);
	qma6981_set_stick_check_reg(0);
	return sprintf(buf, "cic_z_00=%d cic_z_1f=%d\n", cic_z_00, cic_z_1f);
}


static DRIVER_ATTR(chipinfo,    S_IRUGO, show_chipinfo_value, NULL);
static DRIVER_ATTR(waferid,    S_IRUGO, show_waferid_value, NULL);
static DRIVER_ATTR(sensordata, S_IWUSR | S_IRUGO, show_sensordata_value,    NULL);
static DRIVER_ATTR(dumpallreg,  S_IRUGO , show_dumpallreg_value, NULL);
static DRIVER_ATTR(cali,       S_IWUSR | S_IRUGO, show_cali_value,          store_cali_value);
static DRIVER_ATTR(trace,       S_IRUGO | S_IWUSR, show_trace_value, store_trace_value);
static DRIVER_ATTR(layout,      S_IRUGO | S_IWUSR, show_layout_value, store_layout_value);
static DRIVER_ATTR(setreg,   	S_IWUSR | S_IRUGO, NULL,        store_setreg);
static DRIVER_ATTR(getreg,   	S_IWUSR | S_IRUGO, show_getreg,        store_getreg);
static DRIVER_ATTR(cic,   	S_IWUSR | S_IRUGO, show_cic,        NULL);

static struct driver_attribute *qma6981_attr_list[] = {
	&driver_attr_chipinfo,
	&driver_attr_waferid,
	&driver_attr_sensordata,   /*dump sensor data*/
	&driver_attr_cali,         /*show calibration data*/
	&driver_attr_dumpallreg,
	&driver_attr_layout,
	&driver_attr_trace,
	&driver_attr_setreg,
	&driver_attr_getreg,
	&driver_attr_cic
};

static int qma6981_initialize(struct i2c_client *client)
{
	int ret = 0;
	int direction = 0;
	unsigned char data[2] = {0};

	direction = qma6981->hw->direction;
	GSE_LOG("%s: direction %d\n",__FUNCTION__,direction);

	//0x0c lower output data rate ,Bandwidth = 62.5Hz ODR = 2*BW = 125Hz @2g
	//0x0d lower output data rate ,Bandwidth = 125Hz ODR = 2*BW = 250Hz @8g
	//0x0b lower output data rate ,Bandwidth = 31.2Hz ODR = 2*BW = 62.5Hz @8g
	//0x2a high output data rate ,Bandwidth = 15.6Hz ODR = 4*BW =  62.5Hz @8g
	//0x2b high output data rate ,Bandwidth = 31.25Hz ODR = 4*BW = 125Hz @8g	

	data[0] = 0x10;
	data[1] = 0x2a;	//0x2a;	0x0b
	ret = I2C_TxData(data,2);
   	if(ret < 0)
	  goto exit_i2c_err;
  
	//0x01 range  2g , 3.9mg/LSB  @2g
	//0x04 range  8g , 15.6mg/LSB @8g
	data[0] = 0x0F;
	data[1] = 0x02;
// add by yangzhiqiang
	if(data[1] == 0x04)
		qm6981_248g_offset = 6;
	else if(data[1] == 0x02)
		qm6981_248g_offset = 7;
	else if(data[1] == 0x01)
		qm6981_248g_offset = 8;
	else
		qm6981_248g_offset = 8;
// yangzhiqiang
	ret = I2C_TxData(data,2);
   	if(ret < 0)
   		goto exit_i2c_err;

#if defined(QMA6981_WRITE_OFFSET_TO_REG)
	qma6981_WriteOffset(client, qma6981->cali_sw[0], qma6981->cali_sw[1], qma6981->cali_sw[2]);
#else
	data[0] = 0x27;
	data[1] = 0x00;
	ret = I2C_TxData(data,2);

	data[0] = 0x28;
	data[1] = 0x00;
	ret = I2C_TxData(data,2);

	data[0] = 0x29;
	data[1] = 0x00;
	ret = I2C_TxData(data,2);
#endif
	//data[0] = 0x19;
	//data[1] = 0x00;  // step_en ,step_quit_en
	//ret = I2C_TxData(data,2);

	//data[0] = 0x16;
	//data[1] = 0x00;  // step_en ,step_quit_en
	//ret = I2C_TxData(data,2);
	
	//0x11 = 0x8a active mode, sleep time 10ms @2g
	//0x11 = 0x8a active mode, sleep time 10ms @8g
	//0x11 = 0x88 active mode, sleep time 4ms  @8g
	//0x11 = 0x80 active mode, sleep time 0ms  @8g
	data[0] = 0x11;
	data[1] = 0x80;
	ret = I2C_TxData(data,2);

   	return 0;
exit_i2c_err:
	GSE_ERR("qma6981_initialize fail: %d\n",ret);
	return ret;
}

static void qma6981_set_stick_check_reg(int flag)
{
	unsigned char data[2] = {0};
	unsigned char reg_59, reg_5c, reg_5d;
	//short cic_z_00, cic_z_1f;
	int ret;

	qma6981_stick_status = 0;
	if(flag)
	{
		data[0] = 0x5f;
		data[1] = 0x80;
		ret = I2C_TxData(data,2);
		
		data[0] = 0x32;
		data[1] = 0x03;
		ret = I2C_TxData(data,2);

		data[0] = 0x59;
		ret = I2C_RxData(data, 1);
		reg_59 = data[0];
		data[0] = 0x59;
		data[1] = reg_59|0x80;
		ret = I2C_TxData(data,2);

		// read CIC_Z_00
		data[0] = 0x42;
		data[1] = 0x80;
		ret = I2C_TxData(data,2);
		data[0] = 0x5c;
		ret = I2C_RxData(data, 1);
		reg_5c = data[0];
		data[0] = 0x5d;
		ret = I2C_RxData(data, 1);
		reg_5d = data[0];
		cic_z_00 = (short)((reg_5d<<8)|(reg_5c));
		cic_z_00 = cic_z_00>>2;
		
		// read CIC_Z_1F
		data[0] = 0x42;
		data[1] = 0x9f;
		ret = I2C_TxData(data,2);
		data[0] = 0x5c;
		ret = I2C_RxData(data, 1);
		reg_5c = data[0];
		data[0] = 0x5d;
		ret = I2C_RxData(data, 1);
		reg_5d = data[0];
		cic_z_1f = (short)((reg_5d<<8)|(reg_5c));
		cic_z_1f = cic_z_1f>>2;

		printk("qma6981 check stick cic_z_00=%d cic_z_1f=%d \n", cic_z_00, cic_z_1f);
		if(QMA6981_ABS(cic_z_00)>QMA6981_CIC_THRESHOLD || QMA6981_ABS(cic_z_1f)>QMA6981_CIC_THRESHOLD)
		{
			qma6981_stick_status = 1;
		}
		else
		{
			qma6981_stick_status = 0;
		}
	}
	else
	{
		data[0] = 0x36;
		data[1] = 0xb6;	//0x2a;	0x0b
		ret = I2C_TxData(data,2);	
		mdelay(5);
		data[0] = 0x36;
		data[1] = 0x00; //0x2a; 0x0b
		ret = I2C_TxData(data,2);
		data[0] = 0x11;
		data[1] = 0x80; //0x2a; 0x0b
		ret = I2C_TxData(data,2);
		qma6981_initialize(qma6981_i2c_client);
	}
}


/*  ioctl command for qma6981 device file */
static long qma6981_unlocked_ioctl(struct file *file, unsigned int cmd,unsigned long arg)
{
	int err = 0;
	void __user *val;
	char strbuf[qma6981_BUFSIZE];
	int vec[3] = {0};
	struct SENSOR_DATA sensor_data= {0};
	int cali[3];

	struct i2c_client *client = (struct i2c_client*)file->private_data;

	struct qma6981_data *obj = (struct qma6981_data*)i2c_get_clientdata(client);	

	GSE_LOG("qma6981_unlocked_ioctl - cmd=%u, arg = %lu\n" , cmd, arg);
	//GSE_LOG("SET CAL = %d , CLR CAL = %d",GSENSOR_IOCTL_SET_CALI , GSENSOR_IOCTL_CLR_CALI);
	/* check qma6981_client */
	if (&qma6981->client == NULL) {
		GSE_LOG( "I2C driver not install\n");
		return -EFAULT;
	}

	switch (cmd) {
	
	case GSENSOR_IOCTL_INIT:
		qma6981_initialize(client);
		break;
	case GSENSOR_IOCTL_READ_SENSORDATA:

		val = (void __user *) arg;
		if(val == NULL)
		{
			err = -EINVAL;
			break;	  
		}
		
		//GSE_LOG("qma6981_unlocked_ioctl - cmd=GSENSOR_IOCTL_READ_SENSORDATA\n");
		qma6981_read_acc_xyz(vec);
		sprintf(strbuf, "%04x %04x %04x", vec[0], vec[1], vec[2]);
		if(copy_to_user(val, strbuf, strlen(strbuf)+1))
		{
			err = -EFAULT;
			break;	  
		}	
		break;

	case GSENSOR_IOCTL_READ_RAW_DATA:
		val = (void __user *) arg;
		if(val == NULL)
		{
			err = -EINVAL;
			break;	  
		}
		qma6981_read_raw_xyz(vec);
		
		sprintf(strbuf, "%04x %04x %04x", vec[0], vec[1], vec[2]);
		if(copy_to_user(val, strbuf, strlen(strbuf)+1))
		{
			err = -EFAULT;
			break;	  
		}
		break;	  

	case GSENSOR_IOCTL_SET_CALI:
		GSE_LOG("qma6981 ioctl GSENSOR_IOCTL_SET_CALI\n");
		val = (void __user*)arg;
		if(val == NULL)
		{
			err = -EINVAL;
			break;	  
		}
		if(copy_from_user(&sensor_data, val, sizeof(sensor_data)))
		{
			err = -EFAULT;
			break;	  
		}
		if(atomic_read(&obj->suspend))
		{
			GSE_LOG("Perform calibration in suspend state!!\n");
			err = -EINVAL;
		}
		else
		{
			cali[QMA6981_AXIS_X] = (sensor_data.x<<(qm6981_248g_offset))/GRAVITY_EARTH_1000;
			cali[QMA6981_AXIS_Y] = (sensor_data.y<<(qm6981_248g_offset))/GRAVITY_EARTH_1000;
			cali[QMA6981_AXIS_Z] = (sensor_data.z<<(qm6981_248g_offset))/GRAVITY_EARTH_1000;
			//GSE_LOG("QST qma6981 x:%d y:%d z:%d\n", sensor_data.x, sensor_data.y, sensor_data.z);
			//GSE_LOG("QST qma6981 cali[0]:%d cali[1]:%d cali[2]:%d\n", cali[0], cali[1], cali[2]);
			err = qma6981_WriteCalibration(client, cali);
		}
		break;

	case GSENSOR_IOCTL_CLR_CALI:
		//GSE_LOG("qma6981 ioctl GSENSOR_IOCTL_CLR_CALI\n");
		err = qma6981_ResetCalibration(client);
		break;

	case GSENSOR_IOCTL_GET_CALI:
		//GSE_LOG("qma6981 ioctl GSENSOR_IOCTL_GET_CALI\n");
		val = (void __user*)arg;
		if(val == NULL)
		{
			err = -EINVAL;
			break;	  
		}
		if(0 != (err = qma6981_ReadCalibration(client, cali)))
		{
			break;
		}
		//GSE_LOG("qma6981 get cali.x,y,z[%d,%d,%d]\n",cali[QMA6981_AXIS_X],cali[QMA6981_AXIS_Y],cali[QMA6981_AXIS_Z]);
		sensor_data.x = (cali[QMA6981_AXIS_X] * GRAVITY_EARTH_1000)>>(qm6981_248g_offset);
		sensor_data.y = (cali[QMA6981_AXIS_Y] * GRAVITY_EARTH_1000)>>(qm6981_248g_offset);
		sensor_data.z = (cali[QMA6981_AXIS_Z] * GRAVITY_EARTH_1000)>>(qm6981_248g_offset);
		//GSE_LOG("QST qma6981 get sensor_data.x,y,z[%d,%d,%d]\n",sensor_data.x,sensor_data.y,sensor_data.z);
		if(copy_to_user(val, &sensor_data, sizeof(sensor_data)))
		{
			err = -EFAULT;
			break;
		}		
		break;
		
	case GSENSOR_IOCTL_SET_STICK_CHECK_REG:
		qma6981_set_stick_check_reg((int)arg);
		break;
	case GSENSOR_IOCTL_GET_STICK_STATUS:
		val = (void __user*)arg;
		if(val == NULL)
		{
			err = -EINVAL;
			break;	  
		}
		memset(strbuf, 0, sizeof(strbuf));
		sprintf(strbuf, "%d", qma6981_stick_status);
		if(copy_to_user(val, strbuf, strlen(strbuf)+1))
		{
			err = -EFAULT;
			break;	  
		}
		break;

	default:
		break;
	}
	return err;
}

#ifdef CONFIG_COMPAT
static long qma6981_compat_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)
{
    long err = 0;

	void __user *arg32 = compat_ptr(arg);
	
	if (!file->f_op || !file->f_op->unlocked_ioctl)
		return -ENOTTY;
	
    switch (cmd)
    {
        case COMPAT_GSENSOR_IOCTL_READ_SENSORDATA:
            if (arg32 == NULL)
            {
                err = -EINVAL;
                break;    
            }
		
		    err = file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_READ_SENSORDATA, (unsigned long)arg32);
		    if (err){
		        GSE_ERR("GSENSOR_IOCTL_READ_SENSORDATA unlocked_ioctl failed.");
		        return err;
		    }
        break;
	case COMPAT_GSENSOR_IOCTL_SET_CALI:
            if (arg32 == NULL)
            {
                err = -EINVAL;
                break;    
            }
		
		    err = file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_SET_CALI, (unsigned long)arg32);
		    if (err){
		        GSE_ERR("GSENSOR_IOCTL_SET_CALI unlocked_ioctl failed.");
		        return err;
		    }
        break;
        case COMPAT_GSENSOR_IOCTL_GET_CALI:
            if (arg32 == NULL)
            {
                err = -EINVAL;
                break;    
            }
		
		    err = file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_GET_CALI, (unsigned long)arg32);
		    if (err){
		        GSE_ERR("GSENSOR_IOCTL_GET_CALI unlocked_ioctl failed.");
		        return err;
		    }
        break;
        case COMPAT_GSENSOR_IOCTL_CLR_CALI:
            if (arg32 == NULL)
            {
                err = -EINVAL;
                break;    
            }
		
		    err = file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_CLR_CALI, (unsigned long)arg32);
		    if (err){
		        GSE_ERR("GSENSOR_IOCTL_CLR_CALI unlocked_ioctl failed.");
		        return err;
		    }
        break;		
        default:
            GSE_ERR("unknown IOCTL: 0x%08x\n", cmd);
            err = -ENOIOCTLCMD;
        break;

    }

    return err;
}
#endif


/*----------------------------------------------------------------------------*/
static int qma6981_open(struct inode *inode, struct file *file)
{
	file->private_data = qma6981_i2c_client;

	if(file->private_data == NULL)
	{
		GSE_LOG("null pointer!!\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int qma6981_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static const struct file_operations qma6981_fops = {
	.owner = THIS_MODULE,
	.open = qma6981_open,
	.release = qma6981_release,
	.unlocked_ioctl = qma6981_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = qma6981_compat_ioctl,
#endif
};

static struct miscdevice qma6981_miscdevice = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gsensor",
	.fops = &qma6981_fops,
};

static int qma6981_SetPowerCTRL(struct i2c_client *client, bool enable)
{
    int res = 0;
    //struct qma6981_data *obj = i2c_get_clientdata(client);
	
    u8 databuf[2];

    if (enable == sensor_power)
    {
		GSE_LOG("Sensor power status is newest!\n");
        return 0;
    }
    if (enable == false){
		databuf[1]=0x00;
    }else{
		databuf[1]=0x80;
		}
    databuf[0] = QMA6981_REG_POWER_CTL;
    res = I2C_TxData(databuf,2);
    sensor_power = enable;
    mdelay(5);
    return res;    
}

static int qma6981_open_report_data(int open)
{
	return 0;
}

static int qma6981_enable_nodata(int en)
{
    int err = 0;
	GSE_LOG("qma6981_enable_nodata en:%d!\n", en);
	if(((en == 0) && (sensor_power == false)) ||((en == 1) && (sensor_power == true)))
	{
		enable_status = sensor_power;
		GSE_LOG("Gsensor device have updated!\n");
	}
	else
	{
		enable_status = !sensor_power;
		if (atomic_read(&qma6981->suspend) == 0)
		{

			err = qma6981_SetPowerCTRL(qma6981->client, enable_status);
			GSE_LOG("Gsensor not in suspend gsensor_SetPowerMode!, enable_status = %d\n",enable_status);
		}
		else
		{
			GSE_LOG("Gsensor in suspend and can not enable or disable!enable_status = %d\n",enable_status);
		}
	}

    if(err != 0)
	{
		GSE_LOG("gsensor_enable_nodata fail!\n");
		return -1;
	}
	return 0;
}

/*----------------------------------------------------------------------------*/
static int qma6981_SetBWRate(struct i2c_client *client, u8 bwrate)
{
    struct qma6981_data *obj = i2c_get_clientdata(client);
    u8 databuf[2] = {0};    
    int res = 0;

    if( (obj->bandwidth != bwrate) || !(atomic_read(&obj->suspend)) )
    {    
        memset(databuf, 0, sizeof(databuf));    
   
        /* write */
        databuf[1] = databuf[0] | bwrate;
        databuf[0] = QMA6981_REG_BW_ODR;    
    
        res = I2C_TxData(databuf,2);
        if (res < 0)
            return QMA6981_ERR_I2C;

        obj->bandwidth = bwrate;
    }

    return 0;    
}

static int qma6981_set_delay(u64 ns)
{
    int err = 0;
    int value;
	int sample_delay;

    value = (int)ns/1000/1000;
	if(value <= 5){
		sample_delay = QMA6981_ODR_250HZ;
	}
	else if(value <= 10){
		sample_delay = QMA6981_ODR_125HZ;
	}
	else if(value <= 20){
		sample_delay = QMA6981_ODR_62HZ;
	}
	else if(value <= 50){
		sample_delay = QMA6981_ODR_31HZ;
	}
	else if(value <= 100){
		sample_delay = QMA6981_ODR_16HZ;
	}
	else {
		sample_delay = QMA6981_ODR_16HZ;
	}

	err = qma6981_SetBWRate(qma6981->client, sample_delay);

	if(err != 0 )
	{
		GSE_LOG("Set delay parameter error!\n");
        return err;
	}

	return err;
}

static int qma6981_get_data(int* x ,int* y,int* z, int* status)
{
	int vec[3]={0};
	int ret=0;

	ret = qma6981_read_acc_xyz(vec);

	*x = vec[0];
	*y = vec[1];
	*z = vec[2];
	
	*status = SENSOR_STATUS_ACCURACY_HIGH;
	return ret;
}

static int qma6981_delete_attr(struct device_driver *driver)
{
	int idx ,err = 0;
	int num = (int)(sizeof(qma6981_attr_list)/sizeof(qma6981_attr_list[0]));

	if(driver == NULL)
	{
		return -EINVAL;
	}
	for(idx = 0; idx < num; idx++)
	{
		driver_remove_file(driver, qma6981_attr_list[idx]);
	}
	return err;
}

static int qma6981_create_attr(struct device_driver *driver)
{
	int idx, err = 0;
	int num = (int)(sizeof(qma6981_attr_list)/sizeof(qma6981_attr_list[0]));
	if (driver == NULL)
	{
		return -EINVAL;
	}
	for(idx = 0; idx < num; idx++)
	{
		if(0 != (err = driver_create_file(driver, qma6981_attr_list[idx])))
		{
			GSE_LOG("attributes (%s) = %d\n", qma6981_attr_list[idx]->attr.name, err);
			break;
		}
	}
	return err;
}

static int qma6981_i2c_probe(struct i2c_client *client, const struct i2c_device_id *devid)
{
	int err = 0;	
	static struct i2c_client *new_client;
	struct qma6981_data *obj;
	struct acc_control_path acc_ctl = {0};
	struct acc_data_path acc_data = {0};
			
	GSE_FUN();
	obj = kzalloc(sizeof(struct qma6981_data), GFP_KERNEL);
	if (obj == NULL)
	{
		err = -ENOMEM;
		goto exit;
	}

	memset(obj, 0, sizeof(struct qma6981_data));
	client->addr = 0x12;
	obj->hw = hw;
	atomic_set(&obj->layout,obj->hw->direction);
	if(0 != (err = hwmsen_get_convert(obj->hw->direction, &obj->cvt)))
	{
		GSE_ERR("invalid direction: %d\n", obj->hw->direction);
		goto exit;
	}
	
	qma6981 = obj;

	obj->client = client;
	new_client = obj->client;	 
	i2c_set_clientdata(new_client, obj);
	
	qma6981_i2c_client = new_client;

	if(0 > qma6981_initialize(new_client))
	{
		err = -EFAULT;
		goto exit_kfree;
	}

	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
	
	err = misc_register(&qma6981_miscdevice);
	if(err)
	{
		GSE_ERR("%s: misc register failed\n",__FUNCTION__);
		goto exit_kfree;
	}
		
	if(0 != (err = qma6981_create_attr(&qma6981_init_info.platform_diver_addr->driver)))
	{	
		GSE_ERR("%s: create attribute\n",__FUNCTION__);
		goto qma6981_create_attribute_failed;
	}
	
	acc_ctl.open_report_data= qma6981_open_report_data;
	acc_ctl.enable_nodata =qma6981_enable_nodata;
	acc_ctl.set_delay  = qma6981_set_delay;
	acc_ctl.is_report_input_direct = false;
	acc_ctl.is_support_batch = false;

	err = acc_register_control_path(&acc_ctl);
	if(err)
	{
		GSE_ERR("register acc control path err\n");
		goto qma6981_local_init_failed;
	}

	acc_data.get_data = qma6981_get_data;
	acc_data.vender_div = 1000;
	err = acc_register_data_path(&acc_data);
	if(err)
	{
		GSE_ERR("register acc data path err\n");
		goto qma6981_local_init_failed;
	}
	
	err = batch_register_support_info(ID_ACCELEROMETER,acc_ctl.is_support_batch, 1000,0);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	qma6981->early_drv.level    = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,	
	qma6981->early_drv.suspend  = qma6981_early_suspend,	
	qma6981->early_drv.resume   = qma6981_late_resume,    	
	register_early_suspend(&qma6981->early_drv);
#endif

	GSE_LOG("qma6981 device created successfully\n");
	
	qma6981_init_flag = 0;
	return 0;

qma6981_local_init_failed:
	qma6981_delete_attr(&qma6981_init_info.platform_diver_addr->driver);
qma6981_create_attribute_failed:
exit_kfree:
	kfree(obj);
exit:
	qma6981_init_flag = -1;
	return err;
}


static int qma6981_i2c_remove(struct i2c_client *client)
{
	int err = 0;
	
    qma6981_delete_attr(&qma6981_init_info.platform_diver_addr->driver);
	err = misc_deregister(&qma6981_miscdevice);
	if(err)
	{
		GSE_ERR("%s failed\n",__FUNCTION__);
	}
	qma6981_i2c_client = NULL;
	kfree(i2c_get_clientdata(client));
	qma6981 = NULL;
	i2c_unregister_device(client);
	return 0;
}

/*----------------------------------------------------------------------------*/
static void qma6981_power(struct acc_hw *hw, unsigned int on)
{
	static unsigned int power_on = 0;
#if 0
	//GSE_LOG("qma6981_power id:%d on:%d power_on:%d\n",hw->power_id,on,power_on);
	if(hw->power_id != MT65XX_POWER_NONE)
	{
		GSE_LOG("qma6981_power %s\n", on ? "on" : "off");
		if(power_on == on)
		{
			GSE_LOG("qma6981_ignore power control: %d\n", on);
		}
		else if(on)
		{
			GSE_LOG("qma6981_power_A %s\n", on ? "on" : "off");
			if(!hwPowerOn(hw->power_id, hw->power_vol, "qma6981"))
			{
				GSE_LOG( "qma6981_power on fails!!\n");
			}
		}
		else
		{	GSE_LOG("qma6981_power_B %s\n", on ? "on" : "off");
			if(!hwPowerDown(hw->power_id, "qma6981"))
			{
				GSE_LOG( "qma6981_power off fail!!\n");
			}
		}
	}
#endif
	power_on = on;
}



#if !defined(CONFIG_HAS_EARLYSUSPEND)
/*----------------------------------------------------------------------------*/
static int qma6981_suspend(struct i2c_client *client, pm_message_t msg)
{
	struct qma6981_data *obj = i2c_get_clientdata(client);
	GSE_FUN();
	
	if(NULL == obj)
	{
		GSE_LOG("null pointer!!\n");
		return 0;
	}
	if(msg.event == PM_EVENT_SUSPEND)
	{
		GSE_FUN();
		atomic_set(&obj->suspend, 1);
		qma6981_power(obj->hw, 0); //don't power down if enable stepcounter
		qma6981_SetPowerCTRL(obj->client,false);
	}

	return 0;
}

static int qma6981_resume(struct i2c_client *client)
{
	struct qma6981_data *obj = i2c_get_clientdata(client);
	int err;

	GSE_FUN();
	
	if(NULL == obj)
	{
		GSE_LOG("null pointer!!\n");
		return 0;
	}
	qma6981_power(obj->hw, 1);
	qma6981_SetPowerCTRL(obj->client,true);
	err = qma6981_initialize(client);
	if(0 > err)
	{
		GSE_ERR("failed to init qma6981\n");
	}
	atomic_set(&obj->suspend, 0);

	return 0;
}
/*----------------------------------------------------------------------------*/
#else /*CONFIG_HAS_EARLY_SUSPEND is defined*/
/*----------------------------------------------------------------------------*/
static void qma6981_early_suspend(struct early_suspend *h)
{
	struct qma6981_data *obj = container_of(h, struct qma6981_data, early_drv);
	GSE_FUN();
	if(NULL == obj)
	{
		GSE_LOG("null pointer!!\n");
		return;
	}
	atomic_set(&obj->suspend,1);
	qma6981_power(obj->hw, 0); //don't power down if enable stepcounter
	qma6981_SetPowerCTRL(obj->client,false);
}
/*----------------------------------------------------------------------------*/
static void qma6981_late_resume(struct early_suspend *h)
{
	struct qma6981_data *obj = container_of(h, struct qma6981_data, early_drv);
	int err;

	GSE_FUN();
	if(NULL == obj)
	{
		GSE_LOG("null pointer!!\n");
		return;
	}
	atomic_set(&obj->suspend,0);
	qma6981_power(obj->hw, 1);
	qma6981_SetPowerCTRL(obj->client,true);
	err = qma6981_initialize(obj->client);
	if(err)
	{
		GSE_ERR("failed to init qma6981\n");
	}
}
/*----------------------------------------------------------------------------*/
#endif /*CONFIG_HAS_EARLYSUSPEND*/



static const struct i2c_device_id qma6981_id[] = {{QMA6981_ACC_DEV_NAME,0},{}};

#ifdef CONFIG_OF
static const struct of_device_id gsensor_of_match[] = {
	{ .compatible = "mediatek,gsensor"},
	{},
};
#endif

static struct i2c_driver qma6981_driver = {
	.probe = qma6981_i2c_probe,
	.remove = qma6981_i2c_remove,
	.id_table = qma6981_id,
#if !defined(CONFIG_HAS_EARLYSUSPEND)
	.suspend = qma6981_suspend,
	.resume = qma6981_resume,
#endif
	.driver = {
		.owner = THIS_MODULE,
		.name = QMA6981_ACC_DEV_NAME,
#ifdef CONFIG_OF
		.of_match_table = gsensor_of_match,
#endif
	},
};


static int qma6981_local_init(void)
{
	GSE_FUN();
	GSE_LOG("++++++++\n");

	qma6981_power(hw, 1);
	
	if(i2c_add_driver(&qma6981_driver))
	{
		GSE_ERR("add i2c driver error\n");
		return -1;
	}
	if(-1 == qma6981_init_flag)
	{
		GSE_ERR("qma6981 init error\n");
		return -1;
	}		
	
	GSE_LOG("--------\n");
	return 0;
}

static int qma6981_local_uninit(void)
{
    GSE_FUN();
	
    qma6981_power(hw, 0);    
	
    i2c_del_driver(&qma6981_driver);
	
    return 0;
}

static int __init qma6981_init(void)
{
	const char *name = "mediatek,qma6981";
	hw = get_accel_dts_func(name,hw);
	if(!hw){
		GSE_ERR("get cust_accel dts info fail\n");	
	}
	GSE_LOG("%s:i2c_num %d, direction %d, i2c_addr 0x%x\n",__FUNCTION__,hw->i2c_num,hw->direction,hw->i2c_addr[0]);
	acc_driver_add(&qma6981_init_info);

	return 0;
}

static void __exit qma6981_exit(void)
{
	GSE_FUN();
}

module_init(qma6981_init);
module_exit(qma6981_exit);

MODULE_DESCRIPTION("QST qma6981 Acc driver");
MODULE_AUTHOR("QST");
MODULE_LICENSE("GPL");

