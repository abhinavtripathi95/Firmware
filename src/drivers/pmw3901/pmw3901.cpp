/****************************************************************************
 *
 *   Copyright (c) 2013-2017 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file pmw3901.cpp
 * @author Daniele Pettenuzzo
 *
 * Driver for the pmw3901 optical flow sensor connected via SPI.
 */

#include <px4_config.h>
#include <px4_defines.h>
#include <px4_getopt.h>
#include <px4_workqueue.h>

#include <drivers/device/spi.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <semaphore.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <conversion/rotation.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>

#include <drivers/drv_hrt.h>
#include <drivers/drv_range_finder.h>
#include <drivers/device/ringbuffer.h>
#include <drivers/device/integrator.h>

#include <uORB/uORB.h>
#include <uORB/topics/subsystem_info.h>
#include <uORB/topics/optical_flow.h>
#include <uORB/topics/distance_sensor.h>
#include <uORB/topics/sensor_gyro.h>

#include <board_config.h>

/* Configuration Constants */
#ifdef PX4_SPI_BUS_EXT1
#define PMW3901_BUS 							PX4_SPI_BUS_EXT1 			// fmu-v4pro 
#else
#define PMW3901_BUS 							PX4_SPI_BUS_EXTERNAL1		// fmu-v5 
#endif

#ifdef PX4_SPIDEV_EXT0
#define PMW3901_SPIDEV 							PX4_SPIDEV_EXT0 			// fmu-v4pro
#else
#define PMW3901_SPIDEV 							PX4_SPIDEV_EXTERNAL1_1		// fmu-v5 
#endif

#define PMW3901_SPI_BUS_SPEED    				(2000000L)      			// 2MHz

#define DIR_WRITE(a)             				((a) | (1 << 7))
#define DIR_READ(a)                    			((a) & 0x7f)

#define PMW3901_DEVICE_PATH   					"/dev/pmw3901"

/* PMW3901 Registers addresses */
#define PMW3901_CONVERSION_INTERVAL 			1000 						/*   1 ms */
#define PMW3901_SAMPLE_RATE						10000 						/*  10 ms */
#define PMW3901_INTEGRATOR_RESET_TIME			100000						/* 100 ms */


#ifndef CONFIG_SCHED_WORKQUEUE
# error This requires CONFIG_SCHED_WORKQUEUE.
#endif

class PMW3901 : public device::SPI
{
public:
	PMW3901(uint8_t rotation = distance_sensor_s::ROTATION_DOWNWARD_FACING, int bus = PMW3901_BUS);

	virtual ~PMW3901();

	virtual int 		init();

	virtual ssize_t		read(device::file_t *filp, char *buffer, size_t buflen);

	virtual int			ioctl(device::file_t *filp, int cmd, unsigned long arg);

	/**
	* Diagnostics - print some basic information about the driver.
	*/
	void				print_info();

	int					collect();

private:
	uint8_t _rotation;
	work_s						_work;
	ringbuffer::RingBuffer		*_reports;
	bool						_sensor_ok;
	int							_measure_ticks;
	int							_class_instance;
	int							_orb_class_instance;

	orb_advert_t				_optical_flow_pub;
	//int 						_gyro_data_sub;

	perf_counter_t				_sample_perf;
	perf_counter_t				_comms_errors;

	Integrator					_flow_int;

	enum Rotation 				_sensor_rotation;



	/**
	* Initialise the automatic measurement state machine and start it.
	*
	* @note This function is called at open and error time.  It might make sense
	*       to make it more aggressive about resetting the bus in case of errors.
	*/
	void				start();

	/**
	* Stop the automatic measurement state machine.
	*/
	void				stop();

	/**
	* Perform a poll cycle; collect from the previous measurement
	* and start a new one.
	*/
	void				cycle();
	// int					collect();

	int 				readRegister(unsigned reg, uint8_t *data, unsigned count);
  	int         		writeRegister(unsigned reg, uint8_t data);

	int 				sensorInit();
  	int        			readMotionCount(int16_t &deltaX, int16_t &deltaY);

	/**
	* Static trampoline from the workq context; because we don't have a
	* generic workq wrapper yet.
	*
	* @param arg		Instance pointer for the driver that is polling.
	*/
	static void			cycle_trampoline(void *arg);


};




/*
 * Driver 'main' command.
 */
extern "C" __EXPORT int pmw3901_main(int argc, char *argv[]);

PMW3901::PMW3901(uint8_t rotation, int bus) :
	SPI("PMW3901", PMW3901_DEVICE_PATH, bus, PMW3901_SPIDEV, SPIDEV_MODE0, PMW3901_SPI_BUS_SPEED),
	_rotation(rotation),
	_reports(nullptr),
	_sensor_ok(false),
	_measure_ticks(0),
	_class_instance(-1),
	_orb_class_instance(-1),
	_optical_flow_pub(nullptr),
	_sample_perf(perf_alloc(PC_ELAPSED, "tr1_read")),
	_comms_errors(perf_alloc(PC_COUNT, "tr1_com_err")),
	_flow_int(PMW3901_INTEGRATOR_RESET_TIME, false),
	_sensor_rotation((enum Rotation)rotation)
{

	// enable debug() calls
	_debug_enabled = false;

	// work_cancel in the dtor will explode if we don't do this...
	memset(&_work, 0, sizeof(_work));
}

PMW3901::~PMW3901()
{
	/* make sure we are truly inactive */
	stop();

	/* free any existing reports */
	if (_reports != nullptr) {
		delete _reports;
	}

	// if (_class_instance != -1) {
	// 	unregister_class_devname(PMW3901_DEVICE_PATH, _class_instance);
	// }

	// free perf counters
	perf_free(_sample_perf);
	perf_free(_comms_errors);
}


int
PMW3901::sensorInit()
{
	int ret;
	uint8_t data[5];

	// initialize pmw3901 flow sensor
	readRegister(0x00, &data[0], 1); // chip idreadRegister(0x5F, &data[1], 1); // inverse chip id

	// Power on reset
	writeRegister(0x3A, 0x5A);
	usleep(5000);

	// Test the SPI communication, checking chipId and inverse chipId
	if (data[0] != 0x49 && data[1] != 0xB8) return false;

	// Reading the motion registers one time
	readRegister(0x02, &data[0], 1);
	readRegister(0x03, &data[1], 1);
	readRegister(0x04, &data[2], 1);
	readRegister(0x05, &data[3], 1);
	readRegister(0x06, &data[4], 1);

	usleep(1000);

	// set performance optimization registers
	writeRegister(0x7F, 0x00);
 	writeRegister(0x61, 0xAD);
  	writeRegister(0x7F, 0x03);
  	writeRegister(0x40, 0x00);
  	writeRegister(0x7F, 0x05);
  	writeRegister(0x41, 0xB3);
  	writeRegister(0x43, 0xF1);
  	writeRegister(0x45, 0x14);
  	writeRegister(0x5B, 0x32);
  	writeRegister(0x5F, 0x34);
  	writeRegister(0x7B, 0x08);
  	writeRegister(0x7F, 0x06);
  	writeRegister(0x44, 0x1B);
  	writeRegister(0x40, 0xBF);
  	writeRegister(0x4E, 0x3F);
  	writeRegister(0x7F, 0x08);
  	writeRegister(0x65, 0x20);
  	writeRegister(0x6A, 0x18);
  	writeRegister(0x7F, 0x09);
  	writeRegister(0x4F, 0xAF);
  	writeRegister(0x5F, 0x40);
  	writeRegister(0x48, 0x80);
  	writeRegister(0x49, 0x80);
  	writeRegister(0x57, 0x77);
  	writeRegister(0x60, 0x78);
  	writeRegister(0x61, 0x78);
  	writeRegister(0x62, 0x08);
  	writeRegister(0x63, 0x50);
  	writeRegister(0x7F, 0x0A);
  	writeRegister(0x45, 0x60);
  	writeRegister(0x7F, 0x00);
  	writeRegister(0x4D, 0x11);
  	writeRegister(0x55, 0x80);
  	writeRegister(0x74, 0x1F);
  	writeRegister(0x75, 0x1F);
  	writeRegister(0x4A, 0x78);
  	writeRegister(0x4B, 0x78);
  	writeRegister(0x44, 0x08);
  	writeRegister(0x45, 0x50);
  	writeRegister(0x64, 0xFF);
  	writeRegister(0x65, 0x1F);
  	writeRegister(0x7F, 0x14);
  	writeRegister(0x65, 0x60);
  	writeRegister(0x66, 0x08);
  	writeRegister(0x63, 0x78);
  	writeRegister(0x7F, 0x15);
  	writeRegister(0x48, 0x58);
  	writeRegister(0x7F, 0x07);
  	writeRegister(0x41, 0x0D);
  	writeRegister(0x43, 0x14);
  	writeRegister(0x4B, 0x0E);
  	writeRegister(0x45, 0x0F);
  	writeRegister(0x44, 0x42);
  	writeRegister(0x4C, 0x80);
  	writeRegister(0x7F, 0x10);
  	writeRegister(0x5B, 0x02);
  	writeRegister(0x7F, 0x07);
  	writeRegister(0x40, 0x41);
  	writeRegister(0x70, 0x00);

  	usleep(10000);

  	writeRegister(0x32, 0x44);
  	writeRegister(0x7F, 0x07);
  	writeRegister(0x40, 0x40);
  	writeRegister(0x7F, 0x06);
  	writeRegister(0x62, 0xf0);
  	writeRegister(0x63, 0x00);
  	writeRegister(0x7F, 0x0D);
  	writeRegister(0x48, 0xC0);
  	writeRegister(0x6F, 0xd5);
  	writeRegister(0x7F, 0x00);
  	writeRegister(0x5B, 0xa0);
  	writeRegister(0x4E, 0xA8);
  	writeRegister(0x5A, 0x50);
  	writeRegister(0x40, 0x80);

  	ret = OK;

  	return ret;

}

int
PMW3901::init()
{
	int ret = PX4_ERROR;

	/* do I2C init (and probe) first */
	if (SPI::init() != OK) {
		goto out;
	}

  	set_frequency(PMW3901_SPI_BUS_SPEED);

	sensorInit();

	/* allocate basic report buffers */
	_reports = new ringbuffer::RingBuffer(2, sizeof(optical_flow_s));

	if (_reports == nullptr) {
		goto out;
	}

	//_class_instance = register_class_devname(PMW3901_DEVICE_PATH);

	//if (_class_instance == CLASS_DEVICE_PRIMARY) {               // change to optical flow topic
		/* get a publish handle on the range finder topic */
		// struct distance_sensor_s ds_report;

		// _reports->get(&ds_report);

		// _distance_sensor_topic = orb_advertise_multi(ORB_ID(distance_sensor), &ds_report,
		// 			 &_orb_class_instance, ORB_PRIO_LOW);

		// if (_distance_sensor_topic == nullptr) {
		// 	DEVICE_LOG("failed to create distance_sensor object. Did you start uOrb?");
		// }

	//}

	ret = OK;
	_sensor_ok = true;

out:
	return ret;

}


int
PMW3901::ioctl(device::file_t *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

		case SENSORIOCSPOLLRATE: {
				switch (arg) {

					case 0:
						return -EINVAL;

					case SENSOR_POLLRATE_DEFAULT: {

							/* do we need to start internal polling? */
							bool want_start = (_measure_ticks == 0);

							/* set interval for next measurement to minimum legal value */
							_measure_ticks = USEC2TICK(PMW3901_CONVERSION_INTERVAL);

							/* if we need to start the poll state machine, do it */
							if (want_start) {
								start();
							}

							return OK;
					}

					case SENSOR_POLLRATE_MANUAL: {
						
						stop();
						_measure_ticks = 0;
						return OK;
					}

					/* adjust to a legal polling interval in Hz */
					default: {
							/* do we need to start internal polling? */
							bool want_start = (_measure_ticks == 0);

							/* convert hz to tick interval via microseconds */
							unsigned ticks = USEC2TICK(1000000 / arg);

							/* check against maximum rate */
							if (ticks < USEC2TICK(PMW3901_CONVERSION_INTERVAL)) {
								return -EINVAL;
							}

							/* update interval for next measurement */
							_measure_ticks = ticks;

							/* if we need to start the poll state machine, do it */
							if (want_start) {
								start();
							}

							return OK;
					}
				}
		}

		default:
			/* give it to the superclass */
			return SPI::ioctl(filp, cmd, arg);
	}
}

ssize_t
PMW3901::read(device::file_t *filp, char *buffer, size_t buflen)
{
	unsigned count = buflen / sizeof(struct optical_flow_s);
	struct optical_flow_s *rbuf = reinterpret_cast<struct optical_flow_s *>(buffer);
	int ret = 0;

	/* buffer must be large enough */
	if (count < 1) {
		return -ENOSPC;
	}

	/* if automatic measurement is enabled */
	if (_measure_ticks > 0) {

		/*
		 * While there is space in the caller's buffer, and reports, copy them.
		 * Note that we may be pre-empted by the workq thread while we are doing this;
		 * we are careful to avoid racing with them.
		 */
		while (count--) {
			if (_reports->get(rbuf)) {
				ret += sizeof(*rbuf);
				rbuf++;
			}
		}

		/* if there was no data, warn the caller */
		return ret ? ret : -EAGAIN;
	}

	/* manual measurement - run one conversion */
	do {
		_reports->flush();

		/* trigger a measurement */
		if (OK != collect()) {
			ret = -EIO;
			break;
		}

		/* state machine will have generated a report, copy it out */
		if (_reports->get(rbuf)) {
			ret = sizeof(*rbuf);
		}

	} while (0);

	return ret;
}


int
PMW3901::readRegister(unsigned reg, uint8_t *data, unsigned count)
{
	uint8_t cmd[5]; 		// read up to 4 bytes
	int ret;

	cmd[0] = DIR_READ(reg);

	ret = transfer(&cmd[0], &cmd[0], count+1);

	if (OK != ret) {
		perf_count(_comms_errors);
		DEVICE_LOG("spi::transfer returned %d", ret);
		return ret;
	}

	memcpy(&data[0], &cmd[1], count);

	ret = OK;

	return ret;

}


int
PMW3901::writeRegister(unsigned reg, uint8_t data)
{
	uint8_t cmd[2]; 		// write 1 byte
	int ret;

	cmd[0] = DIR_WRITE(reg);
	cmd[1] = data;

	ret = transfer(&cmd[0], nullptr, 2);

	if (OK != ret) {
		perf_count(_comms_errors);
		DEVICE_LOG("spi::transfer returned %d", ret);
		return ret;
	}

	ret = OK;

	return ret;

}


int
PMW3901::collect()
{
	int ret;
  	int16_t deltaX, deltaY;

  	math::Vector<3> 			aval_flow_integrated;
	uint64_t 					integral_dt_flow;

	float x_integral, y_integral;

  	//bool updated = false;
  	//sensor_gyro_s gyro_data;

  	//memset(&gyro_data, 0, sizeof(gyro_data));

  	//int gyro_data_sub = orb_subscribe_multi(ORB_ID(sensor_gyro), 0);


  	perf_begin(_sample_perf);

  	uint64_t timestamp = hrt_absolute_time();

  	readMotionCount(deltaX, deltaY);


  	math::Vector<3> aval_flow(deltaX, deltaY, 0);
  	//math::Vector<3> aval_gyro(gyro_data.x, gyro_data.y, gyro_data.z);

  	bool flow_notify = _flow_int.put(timestamp, aval_flow, aval_flow_integrated, integral_dt_flow);
  	//bool gyro_notify = _gyro_int.put(timestamp, aval_gyro, aval_gyro_integrated, integral_dt_gyro);

    printf("Timestamp = %lld\n", timestamp);
  	printf("deltaX= %d, deltaY= %d\n", deltaX, deltaY);
  	

  	if(flow_notify) {

  		//x_integral = (float)aval_flow_integrated(0) / (float)integral_dt_flow * 1000000.0f * 0.23f;			// proportional factor + convert from pixels to radians
  		//y_integral = (float)aval_flow_integrated(1) / (float)integral_dt_flow * 1000000.0f * 0.23f;			// proportional factor + convert from pixels to radians 

  		x_integral = (float)aval_flow_integrated(0) * 0.23f;			// proportional factor + convert from pixels to radians
  		y_integral = (float)aval_flow_integrated(1) * 0.23f;

  		//while(!updated){
  		//orb_check(gyro_data_sub, &updated);
	  	//}

	  	//if(updated) {
	  		//orb_copy(ORB_ID(sensor_gyro), gyro_data_sub, &gyro_data);
	  		//printf("deltaX= %.3lf, deltaY= %.3lf, deltaZ= %.3lf\n", (double)gyro_data.x, (double)gyro_data.y, (double)gyro_data.z);
	  	//}

  		// x_integral_gyro = (float)aval_gyro_integrated(0) / (float)integral_dt_gyro;
  		// y_integral_gyro = (float)aval_gyro_integrated(1) / (float)integral_dt_gyro;
  		// z_integral_gyro = (float)aval_gyro_integrated(2) / (float)integral_dt_gyro;

  		//printf("Integral: X=%.4lf, Y=%.4lf\n", (double)x_integral, (double)y_integral);
  		//printf("dt = %lld\n\n", integral_dt_flow);

  		struct optical_flow_s report;

  		report.timestamp = hrt_absolute_time();

  		report.pixel_flow_x_integral = static_cast<float>(x_integral); 
  		report.pixel_flow_y_integral = static_cast<float>(y_integral); 

  		//report.gyro_x_rate_integral = static_cast<float>(gyro_data.x);
  		//report.gyro_y_rate_integral = static_cast<float>(gyro_data.y);
  		//report.gyro_z_rate_integral = static_cast<float>(gyro_data.z);

  		report.frame_count_since_last_readout = 10.0f;
  		report.integration_timespan = integral_dt_flow; 				//microseconds

  		report.sensor_id = 0;
  		report.quality = 255;  // TO DO: how do we set this? ekf looks at this in order to see if sample should be used or not!!!

  		/* rotate measurements in yaw from sensor frame to body frame according to parameter SENS_FLOW_ROT */
  		//float zeroval = 0.0f;
		//rotate_3f(_sensor_rotation, report.pixel_flow_x_integral, report.pixel_flow_y_integral, zeroval);

		if (_optical_flow_pub == nullptr) {

			_optical_flow_pub = orb_advertise(ORB_ID(optical_flow), &report);
		} else {

			orb_publish(ORB_ID(optical_flow), _optical_flow_pub, &report);
		}

		/* post a report to the ring */
		_reports->force(&report);

		/* notify anyone waiting for data */
		poll_notify(POLLIN);

  	}

  	//orb_unsubscribe(gyro_data_sub);

  	ret = OK;

	perf_end(_sample_perf);
  	return ret;

}


int 
PMW3901::readMotionCount(int16_t &deltaX, int16_t &deltaY)
{
	// uint8_t tmp;
	// uint8_t dX[2];
	// uint8_t dY[2];
	int ret;

	// readRegister(0x02, &tmp, 1);

	// readRegister(0x03, &dX[0], 1);
	// readRegister(0x04, &dX[1], 1);
	// deltaX = ((int16_t)dX[1] << 8) | dX[0];

	// readRegister(0x05, &dY[0], 1);
	// readRegister(0x06, &dY[1], 1);
	// deltaY = ((int16_t)dY[1] << 8) | dY[0];

	uint8_t data[10] = { DIR_READ(0x02), 0, DIR_READ(0x03), 0, DIR_READ(0x04), 0, 
						DIR_READ(0x05), 0, DIR_READ(0x06), 0 };

	// uint8_t data[5] = { DIR_READ(0x03), 0, 0, 0, 0 };

	// uint8_t data[5] = { DIR_READ(0x16), 0, 0, 0, 0 };

	ret = transfer(&data[0], &data[0], 10);

	if (OK != ret) {
		perf_count(_comms_errors);
		DEVICE_LOG("spi::transfer returned %d", ret);
		return ret;
	}

	deltaX = ((int16_t)data[5] << 8) | data[3];
	deltaY = ((int16_t)data[9] << 8) | data[7];

	// deltaX = ((int16_t)data[2] << 8) | data[1];
	// deltaY = ((int16_t)data[4] << 8) | data[3];

	ret = OK;

	return ret;

}


void
PMW3901::start()
{
	/* reset the report ring and state machine */
	_reports->flush();

	/* schedule a cycle to start things */
	work_queue(HPWORK, &_work, (worker_t)&PMW3901::cycle_trampoline, this, 1000);

	/* notify about state change */
	struct subsystem_info_s info = {};
	info.present = true;
	info.enabled = true;
	info.ok = true;
	info.subsystem_type = subsystem_info_s::SUBSYSTEM_TYPE_OPTICALFLOW;

	static orb_advert_t pub = nullptr;

	if (pub != nullptr) {
		orb_publish(ORB_ID(subsystem_info), pub, &info);

	} else {
		pub = orb_advertise(ORB_ID(subsystem_info), &info);
	}
}

void
PMW3901::stop()
{
	work_cancel(HPWORK, &_work);
}

void
PMW3901::cycle_trampoline(void *arg)
{
	PMW3901 *dev = (PMW3901 *)arg;

	dev->cycle();
}

void
PMW3901::cycle()
{
	collect();

	/* schedule a fresh cycle call when the measurement is done */
	work_queue(HPWORK,
		   &_work,
		   (worker_t)&PMW3901::cycle_trampoline,
		   this,
		   USEC2TICK(PMW3901_SAMPLE_RATE));

}

void
PMW3901::print_info()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);
	printf("poll interval:  %u ticks\n", _measure_ticks);
	_reports->print_info("report queue");
}


/**
 * Local functions in support of the shell command.
 */
namespace pmw3901
{

PMW3901	*g_dev;

void	start(uint8_t rotation);
void	stop();
void	test();
void	reset();
void	info();


/**
 * Start the driver.
 */
void
start(uint8_t rotation)
{
	int fd;

	if (g_dev != nullptr) {
		errx(1, "already started");
	}

	/* create the driver */
	g_dev = new PMW3901(rotation, PMW3901_BUS);

	if (g_dev == nullptr) {
		goto fail;
	}

	if (OK != g_dev->init()) {
		goto fail;
	}

	/* set the poll rate to default, starts automatic data collection */
	fd = open(PMW3901_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		goto fail;
	}

	if (ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		goto fail;
	}

	exit(0);

fail:

	if (g_dev != nullptr) {
		delete g_dev;
		g_dev = nullptr;
	}

	errx(1, "driver start failed");
}

/**
 * Stop the driver
 */
void stop()
{
	if (g_dev != nullptr) {
		delete g_dev;
		g_dev = nullptr;

	} else {
		errx(1, "driver not running");
	}

	exit(0);
}


/**
 * Perform some basic functional tests on the driver;
 * make sure we can collect data from the sensor in polled
 * and automatic modes.
 */
void
test()
{
	//struct distance_sensor_s report;                   // change report type

	if (g_dev != nullptr) {
		errx(1, "already started");
	}

	/* create the driver */
	g_dev = new PMW3901(0, PMW3901_BUS);

	if (g_dev == nullptr) {
		delete g_dev;
		g_dev = nullptr;
	}

	if (OK != g_dev->init()) {
		delete g_dev;
		g_dev = nullptr;

	}

	int fd = open(PMW3901_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		err(1, "%s open failed (try 'vl53lxx start' if the driver is not running", PMW3901_DEVICE_PATH);
	}

	/* start the sensor polling at 2Hz */
	// if (OK != ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT)) {			// change this to read only a few samples 
	// 	errx(1, "failed to set 2Hz poll rate");
	// }

	for(int i = 0; i < 10000; i++){
		g_dev->collect();
		usleep(10000);
	}

	errx(0, "PASS");
}


/**
 * Reset the driver.
 */
void
reset()
{
	int fd = open(PMW3901_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		err(1, "failed ");
	}

	if (ioctl(fd, SENSORIOCRESET, 0) < 0) {
		err(1, "driver reset failed");
	}

	if (ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		err(1, "driver poll restart failed");
	}

	exit(0);
}


/**
 * Print a little info about the driver.
 */
void
info()
{
	if (g_dev == nullptr) {
		errx(1, "driver not running");
	}

	printf("state @ %p\n", g_dev);
	g_dev->print_info();

	exit(0);
}

} // namespace pmw3901


int
pmw3901_main(int argc, char *argv[])
{
	int ch;
	int myoptind = 1;

	const char *myoptarg = nullptr;
	uint8_t rotation = distance_sensor_s::ROTATION_DOWNWARD_FACING;

	if (argc < 2) {
		goto out;
	}

	while ((ch = px4_getopt(argc, argv, "R:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 'R':
			rotation = (uint8_t)atoi(myoptarg);
			PX4_INFO("Setting sensor orientation to %d", (int)rotation);
			break;

		default:
			PX4_WARN("Unknown option!");
		}
	}

	/*
	 * Start/load the driver.
	 */
	if (!strcmp(argv[myoptind], "start")) {
		pmw3901::start(rotation);
	}

	/*
	 * Stop the driver
	 */
	if (!strcmp(argv[myoptind], "stop")) {
		pmw3901::stop();
	}

	/*
	 * Test the driver/device.
	 */
	if (!strcmp(argv[myoptind], "test")) {
		pmw3901::test();
	}

	/*
	 * Reset the driver.
	 */
	if (!strcmp(argv[myoptind], "reset")) {
		pmw3901::reset();
	}

	/*
	 * Print driver information.
	 */
	if (!strcmp(argv[myoptind], "info") || !strcmp(argv[1], "status")) {
		pmw3901::info();
	}

out:

	PX4_ERR("unrecognized command, try 'start', 'test', 'reset' or 'info'");
	return PX4_ERROR;
}
