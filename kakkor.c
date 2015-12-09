#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
//#include <ncurses.h>

#include "comm_uart.h"

typedef enum {MODE_UNDEFINED = 0, MODE_OFF, MODE_CHARGE, MODE_DISCHARGE} mode_t;

const char* mode_names[4] = {"MODE_UNDEFINED", "MODE_OFF", "MODE_CHARGE", "MODE_DISCHARGE"};
const char* short_mode_names[4] = {"UNDEF", "OFF", "CHA", "DSCH"};
const char* mode_commands[4] = {"OFF","OFF","CHA","DSCH"};

typedef enum {STOP_MODE_UNDEFINED = 0, STOP_MODE_CURRENT, STOP_MODE_VOLTAGE} stop_mode_t;

const char* stop_mode_names[3] = {"UNDEFINED", "CURRENT STOP", "VOLTAGE STOP"};
const char* short_stop_mode_names[3] = {"UNDEF", "ISTOP", "VSTOP"};

typedef enum {CCCV_UNDEFINED, MODE_CC, MODE_CV} cccv_t;
const char* short_cccv_names[3] = {"UNDEF", "CC", "CV"};


const char* delim = ";";

#define MAX_PARALLEL_CHANNELS 32
#define MIN_ID 0
#define MAX_ID 255

#define tee(fp, fmt, ...) \
 { \
   printf(fmt, __VA_ARGS__); \
   fprintf(fp, fmt, __VA_ARGS__); \
 }

typedef struct
{
	double current;
	double voltage;
	int const_power_mode;
	double power;

	stop_mode_t stop_mode;
	double stop_current;
	double stop_voltage;
} base_settings_t;

typedef struct
{
	int current;
	int voltage;
	int stop_current;
	int stop_voltage;
} hw_base_settings_t;

typedef struct
{
	int voltage;
	int current;
	int temperature;
	int is_cv;
	int current_setpoint;
	mode_t mode;
	cccv_t cccv;
} hw_measurement_t;

typedef struct
{
	int num_hw_measurements; // Channels
	hw_measurement_t hw_meas[MAX_PARALLEL_CHANNELS];
	double voltage;
	double current;
	double temperature;
	mode_t mode;
	cccv_t cccv;
	double cumul_ah;
	double cumul_wh;
	double resistance;
	int start_time;

} measurement_t;


#define MAX_T_CAL_POINTS 20

typedef struct
{
	char* name;
	char* device_name;
	int fd;
	int num_channels;
	int channels[MAX_PARALLEL_CHANNELS];
	int master_channel_idx; // index to channels[] to show which channel is "master".
	// voltage sense and temperature reading are taken from the master channel.
	// When master channel is in CV mode, its current is copied to other channels
	// every second.

	base_settings_t charge;
	base_settings_t discharge;
	hw_base_settings_t hw_charge;
	hw_base_settings_t hw_discharge;

	measurement_t cur_meas;

	int postcharge_cooldown;
	int postdischarge_cooldown;
	int cooldown_start_time;

	double temperature_stop;

	int cycle_cnt;
	mode_t start_mode;
	mode_t cur_mode;
	mode_t next_mode;
	FILE* log;
	FILE* verbose_log;

	int resistance_on;
	int resistance_on_discharge_too;
	int resistance_interval;
	int resistance_interval_offset;
	int resistance_first_pulse_len;
	int resistance_second_pulse_len;
	double resistance_base_current_mul;
	double resistance_first_pulse_current_mul;
	double resistance_second_pulse_current_mul;
	int resistance_state;
	double resistance_last_v;
	int kludgimus_maximus;
	int resistance_every_cycle;

} test_t;


int num_t_cal_points;
double t_cal[MAX_T_CAL_POINTS][2];

double ntc_to_c(double ntc)
{
	int i;

	if(num_t_cal_points > MAX_T_CAL_POINTS || num_t_cal_points < 2) return -9999.0;

	// to do: extrapolate:
	if(ntc > t_cal[0][0])
		return -999.0;

	if(ntc < t_cal[num_t_cal_points-1][0])
		return 999.0;


	// interpolate:
	for(i = 1; i < num_t_cal_points; i++)
	{
		if(ntc <= t_cal[i-1][0] && ntc >= t_cal[i][0])
		{
			double loc = (t_cal[i-1][0] - ntc) / (t_cal[i-1][0] - t_cal[i][0]);
			return (1.0-loc) * t_cal[i-1][1] + loc * t_cal[i][1];
		}
	}

	return -9999.0;
}

int log_read_cycle_num(char* filename)
{
	int cycle_num = 0;
	FILE* logfile = fopen(filename, "r");
	if(!logfile)
	{
		printf("dbg: logfile NULL\n");
		return 0;
	}

	fseek(logfile, -20, SEEK_END);

	int timeout = 1000;
	while(fgetc(logfile) != '\n')
	{
		fseek(logfile, -2, SEEK_CUR);
		if(!(timeout--))
		{
			printf("log_read_cycle_num: Error: cannot find last line feed");
			fclose(logfile);
			return 0;
		}
	}

	fscanf(logfile, " %u;", &cycle_num);
	if(cycle_num < 0 || cycle_num > 100000)
	{
		printf("log_read_cycle_num: Error: got invalid cycle number\n");
		cycle_num = 0;
	}

	printf("dbg: cycle_num = %d\n", cycle_num);

	fclose(logfile);
	return cycle_num;

}


int start_log(test_t* t)
{
	char buf[512];
	if(strlen(t->name) < 1 || strlen(t->name) > 450)
	{
		t->log = NULL;
		t->verbose_log = NULL;
		printf("Invalid test name, cannot open log files\n");
		return -1;
	}

	sprintf(buf, "%s.log", t->name);

	t->cycle_cnt = log_read_cycle_num(buf);

	t->log = fopen(buf, "a");
	sprintf(buf, "%s_verbose.log", t->name);
	t->verbose_log = fopen(buf, "a");

	fprintf(t->log, "cycle%stime%smode%scc/cv%svoltage%scurrent%stemperature%scumul.Ah%scumul.Wh%sDCresistance\n",
		delim,delim,delim,delim,delim,delim,delim,delim,delim);

	fprintf(t->verbose_log, "cycle%stime%smode%scc/cv%svoltage%scurrent%stemperature%scumul.Ah%scumul.Wh%sDCresistance\n",
		delim,delim,delim,delim,delim,delim,delim,delim,delim);

	fflush(t->log);
	fflush(t->verbose_log);
	return 0;
}

void log_measurement(measurement_t* m, test_t* t, int time)
{
	if(t->log == NULL || t->verbose_log == NULL)
	{
		printf("Warn: log == NULL\n");
		return;
	}
	fprintf(t->log, "%u%s%u%s%s%s%s%s%.3f%s%.2f%s%.3f%s%.4f%s%.3f%s%.2f\n",
		t->cycle_cnt, delim, time, delim, short_mode_names[m->mode], delim, short_cccv_names[m->cccv], delim,
		m->voltage, delim, m->current, delim, m->temperature, delim, m->cumul_ah, delim, m->cumul_wh, delim, m->resistance*1000.0);

	fprintf(t->verbose_log, "%u%s%u%s%s%s%s%s%.4f%s%.3f%s%.4f%s%.5f%s%.4f%s%.3f\n",
		t->cycle_cnt, delim, time, delim, short_mode_names[m->mode], delim, short_cccv_names[m->cccv], delim,
		m->voltage, delim, m->current, delim, m->temperature, delim, m->cumul_ah, delim, m->cumul_wh, delim, m->resistance*1000.0);

	fflush(t->log);
	fflush(t->verbose_log);
}

void print_measurement(measurement_t* m, int time)
{
	if(m->resistance != 0.0)
		printf("time=%u %s %s V=%.3f I=%.2f T=%.1f Ah=%.4f Wh=%.3f                           R measured = %.2f\n",
			time, short_mode_names[m->mode], short_cccv_names[m->cccv],
			m->voltage, m->current, m->temperature, m->cumul_ah, m->cumul_wh, m->resistance*1000.0);
	else
		printf("time=%u %s %s V=%.3f I=%.2f T=%.1f Ah=%.4f Wh=%.3f\n",
			time, short_mode_names[m->mode], short_cccv_names[m->cccv],
			m->voltage, m->current, m->temperature, m->cumul_ah, m->cumul_wh);

//	refresh();
}

#define HW_MIN_VOLTAGE 0
#define HW_MAX_VOLTAGE 7000
#define HW_MIN_CURRENT -26000
#define HW_MAX_CURRENT 26000
#define HW_MIN_TEMPERATURE 0
#define HW_MAX_TEMPERATURE 65535

int set_channel_mode(test_t* test, int channel, mode_t mode)
{
	usleep(5000);
	if(mode != MODE_OFF && mode != MODE_CHARGE && mode != MODE_DISCHARGE)
	{
		printf("Error: invalid mode requested (%d)!\n", mode);
		return -1;
	}
	if(channel < MIN_ID || channel > MAX_ID)
	{
		printf("Error: invalid channel requested (%d)!\n", channel);
		return -1;
	}

	char txbuf[32];
	sprintf(txbuf, "@%u:%s;", channel, mode_commands[mode]);
	char expect[32];
	sprintf(expect, "%s OK", mode_commands[mode]);

	if(comm_autoretry(test->fd, txbuf, expect, NULL))
	{
		printf("Emergency: failed to set channel %d to mode %s!\n", channel, mode_names[mode]);
		return -1;
	}

	return 0;
}

int set_test_mode(test_t* test, mode_t mode)
{
	int i;
	int fail = 0;
	for(i = 0; i < test->num_channels; i++)
	{
		usleep(5000);
//		printf("INFO: Setting channel %d to mode %d\n", test->channels[i], mode);
		if(set_channel_mode(test, test->channels[i], mode))
		{
			printf("Error setting channel mode!\n");
			fail = -1;
		}
	}

	test->cur_mode = mode;
	return fail;
}

int configure_hw(test_t* params, mode_t mode);
int translate_settings(test_t* params);

int translate_configure_channel_hws(test_t* test, mode_t mode)
{
	int fail = 0;
	if(translate_settings(test))
		return -1;

	if(configure_hw(test, mode))
		fail = -2;

	return fail;
}

int get_channel_idx(test_t* test, int channel_id)
{
	int i;
	for(i = 0; i < test->num_channels; i++)
	{
		if(channel_id == test->channels[i])
			return i;
	}
	printf("Error: channel index not found with channel id %d\n", channel_id);
	return -1;
}

int clear_hw_measurements(test_t* test)
{
	test->cur_meas.num_hw_measurements = 0;
	return 0;
}

int add_measurement(test_t* test, int channel_id, hw_measurement_t* hw)
{
	int idx = get_channel_idx(test, channel_id);
	if(idx < 0)
		return idx;
	memcpy(&test->cur_meas.hw_meas[idx], hw, sizeof(hw_measurement_t));
	test->cur_meas.num_hw_measurements++;
	return 0;
}

int parse_hw_measurement(hw_measurement_t* meas, char* str)
{
	char* p_val;

//	now checked using comm_autoretry():
//	if(strstr(str, "MEAS") != str)
//		return -1;

//	printf("dbg: parse_hw_measurement(): input: %s.\n", str);
	memset(meas, 0, sizeof(hw_measurement_t));

	if(strstr(str, "OFF"))
		meas->mode = MODE_OFF;
	else if(strstr(str, "CHA"))
		meas->mode = MODE_CHARGE;
	else if(strstr(str, "DSCH"))
		meas->mode = MODE_DISCHARGE;

	if(strstr(str, "CV"))
		meas->cccv = MODE_CV;
	else if(strstr(str, "CC"))
		meas->cccv = MODE_CC;

	if(meas->mode == MODE_UNDEFINED)
		return -2;
	if(meas->cccv == CCCV_UNDEFINED)
		return -3;

	if((p_val = strstr(str, "V=")))
	{
		if(sscanf(p_val, "V=%u", &meas->voltage) != 1)
			return -4;
		if(meas->voltage < HW_MIN_VOLTAGE || meas->voltage > HW_MAX_VOLTAGE)
			return -5;
//		printf("dbg: %u\n", meas->voltage);
	}

	if((p_val = strstr(str, "I=")))
	{
		if(sscanf(p_val, "I=%d", &meas->current) != 1)
			return -6;
		if(meas->current < HW_MIN_CURRENT || meas->current > HW_MAX_CURRENT)
			return -7;
	}

	if((p_val = strstr(str, "T=")))
	{
		if(sscanf(p_val, "T=%u", &meas->temperature) != 1)
			return -8;
		if(meas->temperature < HW_MIN_TEMPERATURE || meas->temperature > HW_MAX_TEMPERATURE)
			return -9;
	}

	if((p_val = strstr(str, "Iset=")))
	{
		if(sscanf(p_val, "Iset=%d", &meas->current_setpoint) != 1)
			return -10;
		if(meas->current_setpoint < HW_MIN_CURRENT || meas->current_setpoint > HW_MAX_CURRENT)
			return -11;
	}

	return 0;
}

void print_params(test_t* params)
{
	int i;
	fprintf(params->verbose_log, "%u parallel channels: ", params->num_channels);
	for(i = 0; i < params->num_channels; i++)
		fprintf(params->verbose_log, "%u  ", params->channels[i]);

	fprintf(params->verbose_log, "Master channel will be: ");
	fprintf(params->verbose_log, "%u\n", params->channels[params->master_channel_idx]);

	fprintf(params->verbose_log, "CHARGE: current=%.3f   voltage=%.3f   stop_mode=%s   stop_current=%.3f   stop_voltage=%.3f\n",
		params->charge.current, params->charge.voltage, short_stop_mode_names[params->charge.stop_mode], params->charge.stop_current, params->charge.stop_voltage);
	fprintf(params->verbose_log, "DISCHARGE: current=%.3f   voltage=%.3f   stop_mode=%s   stop_current=%.3f   stop_voltage=%.3f\n",
		params->discharge.current, params->discharge.voltage, short_stop_mode_names[params->discharge.stop_mode], params->discharge.stop_current, params->discharge.stop_voltage);

	fprintf(params->verbose_log, "HW CHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_charge.current, params->hw_charge.stop_current, params->hw_charge.voltage, params->hw_charge.stop_voltage);

	fprintf(params->verbose_log, "HW DISCHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_discharge.current, params->hw_discharge.stop_current, params->hw_discharge.voltage, params->hw_discharge.stop_voltage);

	fprintf(params->verbose_log, "cycling_start=%s   postcharge_cooldown=%u sec   postdischarge_cooldown=%u sec\n",
		mode_names[params->start_mode], params->postcharge_cooldown, params->postdischarge_cooldown);

	fprintf(params->verbose_log, "temperature_stop=%f\n", params->temperature_stop);

	fprintf(params->verbose_log, "resistance_on=%d, interval=%d, interval_offset=%d first_pulse=%d, second_pulse=%d, base_curr=%f, first_curr=%f, second_curr=%f, resistance_every_cycle=%d\n",
		params->resistance_on, params->resistance_interval, params->resistance_interval_offset, params->resistance_first_pulse_len, params->resistance_second_pulse_len, 
 		params->resistance_base_current_mul, params->resistance_first_pulse_current_mul, params->resistance_second_pulse_current_mul,
		params->resistance_every_cycle);

	fflush(params->log);
	fflush(params->verbose_log);

}

int update_measurement(test_t* test, double elapsed_seconds)
{
//	printf("upd_meas: %u , %u\n", test->master_channel_idx, test->cur_meas.hw_meas[test->master_channel_idx].voltage);
	test->cur_meas.voltage = test->cur_meas.hw_meas[test->master_channel_idx].voltage / 1000.0;
	test->cur_meas.temperature = ntc_to_c(test->cur_meas.hw_meas[test->master_channel_idx].temperature);

	double current_sum = 0;
	int num_channels_in_mode[4] = {0,0,0,0};
	int num_channels_in_cccv[3] = {0,0,0};
	int ch;

	for(ch = 0; ch < test->num_channels; ch++)
	{
		current_sum += (double)test->cur_meas.hw_meas[ch].current / 1000.0;

		int chmode = test->cur_meas.hw_meas[ch].mode;
		if(chmode < 1 || chmode > 3)
		{
			printf("Channel %d in illegal mode (%d)!\n", test->channels[ch], chmode);
			return -1;
		}
		num_channels_in_mode[chmode]++;

		int chcccv = test->cur_meas.hw_meas[ch].cccv;
		if(chcccv < 1 || chcccv > 2)
		{
			printf("Channel %d in illegal CC-CV state (%d)!\n", test->channels[ch], chcccv);
			return -1;
		}
		num_channels_in_cccv[chcccv]++;
	}

	if(num_channels_in_mode[MODE_CHARGE] && num_channels_in_mode[MODE_DISCHARGE])
	{
		printf("Error: channels both in charge & discharge mode!\n");
		return -1;
	}

	if(num_channels_in_mode[MODE_CHARGE])
		test->cur_meas.mode = MODE_CHARGE;
	if(num_channels_in_mode[MODE_DISCHARGE])
		test->cur_meas.mode = MODE_DISCHARGE;

	// todo: fix both charge & discharge.

	if(num_channels_in_mode[MODE_OFF] > 0)
		test->cur_meas.mode = MODE_OFF;

//	printf("num_channels_in_off = %d, num_channels_in_charge = %d, num_channels_in_discharge = %d\n", num_channels_in_mode[MODE_OFF], num_channels_in_mode[MODE_CHARGE], num_channels_in_mode[MODE_DISCHARGE]);
//	printf("------> MODE = %s\n", mode_names[test->cur_meas.mode]);

	if(num_channels_in_cccv[MODE_CV])
		test->cur_meas.cccv = MODE_CV;
	else
		test->cur_meas.cccv = MODE_CC;

	test->cur_meas.current = current_sum;
	test->cur_meas.cumul_ah += current_sum * elapsed_seconds / 3600.0;
	test->cur_meas.cumul_wh += current_sum * test->cur_meas.voltage * elapsed_seconds / 3600.0;


	return 0;
}

int hw_set_current(int fd, int channel, int current);

int measure_hw(test_t* test, int horrible_kludge)
{
	char txbuf[100];
	char expectbuf[100];
	char rxbuf[1000];
	int i;
	int ret;

	uart_flush(test->fd);
	for(i = 0; i < test->num_channels; i++)
	{
		sprintf(txbuf, "@%u:VERB;", test->channels[i]);
		sprintf(expectbuf, "%u:MEAS ", test->channels[i]);
		if(comm_autoretry(test->fd, txbuf, expectbuf, rxbuf))
		{
			printf("Error getting measurement data\n");
			return -1;
		}

		printf("measure_hw: from %3u: %s\n", test->channels[i], rxbuf);
		fprintf(test->verbose_log, "measure_hw: from %3u: %s\n", test->channels[i], rxbuf);

		hw_measurement_t meas;
		if((ret = parse_hw_measurement(&meas, rxbuf)))
		{
			printf("Error: parse_hw_measurement returned %d\n", ret);
			return -1;
		}

		if((ret = add_measurement(test, test->channels[i], &meas)))
		{
			printf("Error: add_measurement returned %d\n", ret);
			return -1;
		}

		if(i == test->master_channel_idx && meas.cccv == MODE_CV && !horrible_kludge)
		{
			printf("Info: Master in CV - copying master current (%d mA) to slaves... ", meas.current_setpoint); fflush(stdout);
			if(meas.current_setpoint < HW_MIN_CURRENT || meas.current_setpoint > HW_MAX_CURRENT ||
			   (meas.mode == MODE_CHARGE && meas.current_setpoint < test->hw_charge.stop_current) ||
			   (meas.mode == MODE_DISCHARGE && meas.current_setpoint > test->hw_discharge.stop_current))
			{
				printf("Illegal master current setpoint (%d mA), aborting copy.\n", meas.current_setpoint);
				return -1;
			}
			int ch;
			for(ch = 0; ch < test->num_channels; ch++)
			{
				if(ch == test->master_channel_idx)
					continue;
				printf(" %d  ", test->channels[ch]);
				if(hw_set_current(test->fd, test->channels[ch], meas.current_setpoint))
				{
					printf("Error: Cannot set current. ");
				}
			}
			printf("\n");
		}

	}

	if(test->cur_meas.num_hw_measurements != test->num_channels)
	{
		printf("Error: didn't get measurements from all channels\n");
		return -1;
	}

	return 0;
}

int hw_set_current(int fd, int channel, int current)
{
	char buf[32];
	if(channel < 0 || channel > MAX_ID || current < HW_MIN_CURRENT || current > HW_MAX_CURRENT)
		go_fatal(fd, "illegal set current");
//		return -2;
	sprintf(buf, "@%u:SETI %d;", channel, current);
	if(comm_autoretry(fd, buf, "SETI OK", NULL))
		return -1;
	return 0;
}

int test_set_current(test_t* test, double current)
{
	int ch;
	for(ch = 0; ch < test->num_channels; ch++)
	{
		if(hw_set_current(test->fd, test->channels[ch], current*1000.0/(double)test->num_channels))
		{
			printf("Error: Cannot set current. ");
			return -1;
		}
	}
	return 0;
}

int configure_hw(test_t* params, mode_t mode)
{
	char buf[200];
	int i;
	hw_base_settings_t* settings;
	if(mode == MODE_CHARGE)
		settings = &params->hw_charge;
	else if(mode == MODE_DISCHARGE)
		settings = &params->hw_discharge;
	else
	{
		printf("Illegal mode (%d) supplied to configure_hw()\n", mode);
		return -2;
	}

	uart_flush(params->fd);
	for(i = 0; i < params->num_channels; i++)
	{
		int extra_vstop = 0;
		int extra_vcv = 200;

		if(i == params->master_channel_idx)
		{
//			printf("Info: Channel %d is master.\n", params->channels[i]);
			extra_vstop = 0;
			extra_vcv = 0;

		}
		else
		{
			if(mode == MODE_DISCHARGE)
			{
				extra_vstop += 500;
				extra_vcv += 200;
			}
		}

		if(mode == MODE_DISCHARGE)
		{
			extra_vstop *= -1;
			extra_vcv *= -1;
		}

		printf("Info: configuring channel %u\n", params->channels[i]);
		sprintf(buf, "@%u:OFF;", params->channels[i]);
		fprintf(params->verbose_log, "    %s", buf);
		if(comm_autoretry(params->fd, buf, "OFF OK", NULL))
			return -1;

		usleep(5000);

		sprintf(buf, "@%u:SETI %d;", params->channels[i], settings->current);
		printf("      %s", buf); fflush(stdout);
		fprintf(params->verbose_log, "    %s", buf);
		if(comm_autoretry(params->fd, buf, "SETI OK", NULL))
			return -1;

		usleep(5000);

		sprintf(buf, "@%u:SETV %d;", params->channels[i], settings->voltage+extra_vcv);
		printf("      %s", buf); fflush(stdout);
		fprintf(params->verbose_log, "    %s", buf);
		if(comm_autoretry(params->fd, buf, "SETV OK", NULL))
			return -1;

		usleep(5000);

		sprintf(buf, "@%u:SETISTOP %d;", params->channels[i], settings->stop_current);
		printf("      %s", buf); fflush(stdout);
		fprintf(params->verbose_log, "    %s", buf);
		if(comm_autoretry(params->fd, buf, "SETISTOP OK", NULL))
			return -1;

		usleep(5000);

		sprintf(buf, "@%u:SETVSTOP %d;", params->channels[i], settings->stop_voltage+extra_vstop);
		printf("      %s\n", buf);
		fprintf(params->verbose_log, "    %s\n", buf);
		if(comm_autoretry(params->fd, buf, "SETVSTOP OK", NULL))
			return -1;

		usleep(5000);

	}

	return 0;
}

int translate_settings(test_t* params)
{
	params->hw_charge.current = (int)( ((params->resistance_on && (params->cycle_cnt % params->resistance_every_cycle)==0)?(params->resistance_base_current_mul):(1.0)) * params->charge.current*1000.0/params->num_channels);
	if(params->charge.stop_mode == STOP_MODE_CURRENT)
	{
		params->hw_charge.stop_current = (int)(params->charge.stop_current*1000.0/params->num_channels);
		params->hw_charge.voltage = (int)(params->charge.voltage*1000.0);
		// In current stop mode, stop voltage will be set so that it's never met - HW does not use
		// "stop modes" at all. Stop voltage will be used as a kind of safety measure.
		params->hw_charge.stop_voltage = (int)((params->charge.voltage+0.13)*1000.0);
	}
	else if(params->charge.stop_mode == STOP_MODE_VOLTAGE)
	{
		params->hw_charge.stop_voltage = (int)(params->charge.stop_voltage*1000.0);
		// Same here - stop mode is achieved by having stop voltage lower than CV voltage.
		params->hw_charge.voltage = (int)((params->charge.stop_voltage+0.13)*1000.0);
		params->hw_charge.stop_current = 1; // stop current will never be met.
	}
	else
	{
		printf("ERROR: translate_settings: illegal stop_mode\n");
		return 1;
	}

	params->hw_discharge.current = -1*((int)( ((params->resistance_on && (params->cycle_cnt % params->resistance_every_cycle)==0)?(params->resistance_base_current_mul):(1.0)) * params->discharge.current*1000.0/params->num_channels));
	if(params->discharge.stop_mode == STOP_MODE_CURRENT)
	{
		params->hw_discharge.stop_current = -1*((int)(params->discharge.stop_current*1000.0/params->num_channels));
		params->hw_discharge.voltage = (int)(params->discharge.voltage*1000.0);
		params->hw_discharge.stop_voltage = (int)((params->discharge.voltage-0.5)*1000.0);
	}
	else if(params->discharge.stop_mode == STOP_MODE_VOLTAGE)
	{
		params->hw_discharge.stop_voltage = (int)(params->discharge.stop_voltage*1000.0);

//		printf("dbg: TRANSLATE -- %s : %u\n", params->name, params->hw_discharge.stop_voltage);

		params->hw_discharge.voltage = (int)((params->discharge.stop_voltage-0.5)*1000.0);
		params->hw_discharge.stop_current = -1;
	}
	else
	{
		printf("ERROR: translate_settings: illegal stop_mode\n");
		return 1;
	}

	return 0;

}

#define MIN_CURRENT 0.2
#define MAX_CURRENT 1000.0

int check_base_settings(char* name, base_settings_t* params)
{
	if(params->current == 0.0 && !params->const_power_mode)
	{
		printf("ERROR: %s: Current missing\n", name);
		return 1;
	}

	if(params->voltage == 0.0)
	{
		if(params->stop_voltage == 0.0)
		{
			printf("ERROR: %s: both voltage and stop voltage missing.\n", name);
			return 1;
		}
		else
		{
			if(params->stop_mode == STOP_MODE_CURRENT)
			{
				printf("ERROR: %s: stop_mode=current specified, but voltage missing\n.", name);
				return 1;
			}
			else
			{
				printf("INFO: %s: inferring stop_mode=voltage\n", name);
				params->stop_mode = STOP_MODE_VOLTAGE;
			}
		}
	}
	else if(params->stop_voltage == 0.0)
	{
		if(params->stop_mode == STOP_MODE_VOLTAGE)
		{
			printf("ERROR: %s: stop_mode=voltage specified, but stopvoltage missing\n", name);
			return 1;
		}
		else
		{
			printf("INFO: %s: inferring stop_mode=current\n", name);
			params->stop_mode = STOP_MODE_CURRENT;
			if(params->stop_current == 0.0)
			{
				printf("ERROR: %s: stopcurrent missing\n", name);
				return 1;
			}
		}
	}
	else
	{
		printf("ERROR: %s: both voltage and stopvoltage defined\n", name);
		return 1;
	}

	if(params->const_power_mode)
	{
		if(params->stop_mode != STOP_MODE_VOLTAGE)
		{
			printf("ERROR: %s: Constant power mode requires stopmode=voltage.\n", name);
			return 1;
		}

	}


	if(params->current < MIN_CURRENT || params->current > MAX_CURRENT)
	{
		printf("ERROR: %s: Illegal current: %.3f\n", name, params->current);
		return 1;
	}


	return 0;
}

int check_params(test_t* params)
{
	int i;

	if(params->num_channels < 1 || params->num_channels > MAX_PARALLEL_CHANNELS)
	{
		printf("Illegal number of parallel channels: %u\n", params->num_channels);
		return -1;
	}

	for(i = 0; i < params->num_channels; i++)
	{
		if(params->channels[i] < MIN_ID || params->channels[i] > MAX_ID)
		{
			printf("Illegal channel ID: %u\n", params->channels[i]);
			return -1;
		}
	}

	if(params->resistance_on)
	{
		if(params->resistance_interval < 20 || params->resistance_interval > 600)
		{
			printf("ERROR: Illegal resistance_interval (%u)\n", params->resistance_interval);
			return -1;
		}
		params->resistance_base_current_mul = 1.0 /
			(((double)(params->resistance_first_pulse_len)*(params->resistance_first_pulse_current_mul) +
			(double)(params->resistance_second_pulse_len)*(params->resistance_second_pulse_current_mul) +
			(double)(params->resistance_interval - params->resistance_first_pulse_len - params->resistance_second_pulse_len)*1.0)
			/(double)(params->resistance_interval));

		if(params->resistance_base_current_mul < 1.0 || params->resistance_base_current_mul > 1.2)
		{
			printf("ERROR: Inferred resistance_base_current_mul out of range (%f)\n", params->resistance_base_current_mul);
			return -1;
		}

		if(params->resistance_every_cycle < 1 || params->resistance_every_cycle > 100)
		{
			printf("ERROR: Illegal resistance_every_cycle (%u)\n", params->resistance_every_cycle);
			return -1;
		}

		printf("INFO: Resistance measurement on, inferring resistance_base_current_mul = %f\n", params->resistance_base_current_mul);
	}

	if(params->discharge.const_power_mode)
	{
		if(params->resistance_on_discharge_too)
		{
			printf("ERROR: Cannot use const power mode when \"resistance=on\". Use \"resistance=charge\".\n");
			return -1;
		}
		double cur_per_ch = params->discharge.power / params->discharge.stop_voltage / params->num_channels;
		printf("INFO: Constant power: maximum current per channel = %f at end voltage %f\n", cur_per_ch, params->discharge.stop_voltage);
		if(cur_per_ch > ((double)(HW_MAX_CURRENT-200)/1000.0))
		{
			printf("ERROR: Constant power: maximum channel current is exceeded!\n");
			return 1;
		}

		double start_cur = params->discharge.power / params->discharge.stop_voltage * 0.7;
		printf("INFO: Constant power: inferring default (starting) current=%f", start_cur);
		params->discharge.current = start_cur;

	}

	if(check_base_settings("Charge", &params->charge))
		return -1;
	if(check_base_settings("Discharge", &params->discharge))
		return -1;


	return 0;
}


int parse_token(char* token, test_t* params)
{
	static mode_t param_state = MODE_OFF;
	int n;
	int itmp;
	double ftmp, ftmp2;
	char ctmp;
	if(strstr(token, "charge") == token)
	{
		param_state = MODE_CHARGE;
		return 0;
	}
	else if(strstr(token, "discharge") == token)
	{
		param_state = MODE_DISCHARGE;
		return 0;
	}
	else if(strstr(token, "device=") == token)
	{
		int arglen;
		if(params->device_name != NULL)
		{
			printf("Note: overriding existing device name (%s)\n", params->device_name);
			free(params->device_name);
		}
		arglen = strlen(token+strlen("device="));
		if((params->device_name = malloc(arglen+1)) == NULL)
		{
			printf("Memory allocation error\n");
			return -1;
		}
		strcpy(params->device_name, token+strlen("device="));
		return 0;
	}
	else if(strstr(token, "startmode=charge") == token)
	{
		params->start_mode=MODE_CHARGE;
		return 0;
	}
	else if(strstr(token, "startmode=discharge") == token)
	{
		params->start_mode=MODE_DISCHARGE;
		return 0;
	}
	else if(sscanf(token, "channels=%u%n", &params->channels[0], &n) == 1)
	{
		token+=n;
		params->num_channels = 1;
		while(sscanf(token, ",%u%n", &params->channels[params->num_channels], &n) == 1)
		{
			token+=n;
			params->num_channels++;
			if(params->num_channels >= MAX_PARALLEL_CHANNELS)
			{
				printf("Too many parallel channels defined in channels list\n");
				return 1;
			}
		}
		return 0;
	}
	else if(sscanf(token, "masterchannel=%u", &itmp) == 1)
	{
		if(itmp >= 0 && itmp < 10000)
		{
			int i;
			for(i = 0; i < params->num_channels; i++)
			{
				if(params->channels[i] == itmp)
				{
					params->master_channel_idx = i;
					goto MASTER_CHANNEL_PARSE_OK;
				}
			}
			printf("Masterchannel value not found in channel list.\n");
			return 1;
			MASTER_CHANNEL_PARSE_OK:
			;
		}
		else
		{
			printf("Illegal masterchannel value.\n");
			return 1;
		}
	}
	else if(sscanf(token, "startcycle=%u", &itmp) == 1)
	{
		if(itmp >= 0 && itmp < 100000)
		{
			params->cycle_cnt = itmp;
		}
		else
		{
			printf("Illegal masterchannel value.\n");
			return 1;
		}
	}
	else if(sscanf(token, "current=%lf", &ftmp) == 1)
	{
		if(param_state == MODE_CHARGE)
			params->charge.current = ftmp;
		else if(param_state == MODE_DISCHARGE)
			params->discharge.current = ftmp;
		else
		{
			printf("current token without charge/discharge keyword before.\n");
			return 1;
		}
	}
	else if(sscanf(token, "voltage=%lf", &ftmp) == 1)
	{
		if(param_state == MODE_CHARGE)
			params->charge.voltage = ftmp;
		else if(param_state == MODE_DISCHARGE)
			params->discharge.voltage = ftmp;
		else
		{
			printf("voltage token without charge/discharge keyword before.\n");
			return 1;
		}
	}
	else if(sscanf(token, "power=%lf", &ftmp) == 1)
	{
		if(param_state == MODE_CHARGE)
		{
			params->charge.power = ftmp;
			params->charge.const_power_mode = 1;
		}
		else if(param_state == MODE_DISCHARGE)
		{
			params->discharge.power = ftmp;
			params->discharge.const_power_mode = 1;
		}
		else
		{
			printf("power token without charge/discharge keyword before.\n");
			return 1;
		}
	}
	else if(sscanf(token, "stopcurrent=%lf", &ftmp) == 1)
	{
		if(param_state == MODE_CHARGE)
			params->charge.stop_current = ftmp;
		else if(param_state == MODE_DISCHARGE)
			params->discharge.stop_current = ftmp;
		else
		{
			printf("stopcurrent token without charge/discharge keyword before.\n");
			return 1;
		}
	}
	else if(sscanf(token, "stopvoltage=%lf", &ftmp) == 1)
	{
		if(param_state == MODE_CHARGE)
			params->charge.stop_voltage = ftmp;
		else if(param_state == MODE_DISCHARGE)
			params->discharge.stop_voltage = ftmp;
		else
		{
			printf("stopvoltage token without charge/discharge keyword before.\n");
			return 1;
		}
	}
	else if(sscanf(token, "temperaturestop>%lf", &ftmp) == 1)
	{
		if(ftmp	< 10.0 || ftmp > 120.0)
		{
			printf("Warning: ignored illegal temperaturestop value.\n");
		}
		else
		{
			params->temperature_stop = ftmp;
		}
	}
	else if((sscanf(token, "cooldown=%d%c", &itmp, &ctmp) == 2) && (ctmp == 's' || ctmp == 'S' || ctmp == 'm' || ctmp == 'M'))
	{
		if(ctmp == 'm' || ctmp == 'M')
			itmp *= 60;

		if(param_state == MODE_CHARGE)
			params->postcharge_cooldown = itmp;
		else if(param_state == MODE_DISCHARGE)
			params->postdischarge_cooldown = itmp;
		else
		{
			printf("Warning: global (non-charge/discharge) cooldown token ignored.\n");
		}

	}
	else if(strstr(token, "resistance=on") == token)
	{
		params->resistance_on=1;
		params->resistance_on_discharge_too=1;
		return 0;
	}
	else if(strstr(token, "resistance=off") == token)
	{
		params->resistance_on=0;
		params->resistance_on_discharge_too=0;
		return 0;
	}
	else if(strstr(token, "resistance=charge") == token)
	{
		params->resistance_on=1;
		params->resistance_on_discharge_too=0;
		return 0;
	}
	else if((sscanf(token, "resistanceinterval=%d%c", &itmp, &ctmp) == 2) && (ctmp == 's' || ctmp == 'S' || ctmp == 'm' || ctmp == 'M'))
	{
		if(ctmp == 'm' || ctmp == 'M')
			itmp *= 60;

		if(itmp < 20 || itmp > 600)
			printf("Warning: ignored out-of-range resistanceinterval (%u)\n", itmp);
		else
		{
			params->resistance_interval = itmp;
			params->resistance_interval_offset = itmp/2;
		}
	}
	else if((sscanf(token, "resistancepulse=%d%c", &itmp, &ctmp) == 2) && (ctmp == 's' || ctmp == 'S' || ctmp == 'm' || ctmp == 'M'))
	{
		if(ctmp == 'm' || ctmp == 'M')
			itmp *= 60;

		if(itmp < 6 || itmp > 60)
			printf("Warning: ignored out-of-range resistancepulse (%u)\n", itmp);
		else
		{
			params->resistance_first_pulse_len = 3;
			params->resistance_second_pulse_len = itmp-3;
		}
	}
	else if(sscanf(token, "resistancecurrent=%lf", &ftmp) == 1)
	{
		if(ftmp < 0.5 || ftmp > 0.95)
		{
			printf("Warning: ignoring out-of-range resistancecurrent (%f)\n", ftmp);
		}
		else
		{
			params->resistance_second_pulse_current_mul = ftmp;
			params->resistance_first_pulse_current_mul = ftmp * 0.85;
		}
	}
	else if(sscanf(token, "resistancecycle=%u", &itmp) == 1)
	{
		if(itmp < 1 || itmp > 1000)
		{
			printf("Warning: ignoring out-of-range resistancecycle (%u) - using 1\n", itmp);
			itmp = 1;
		}
		params->resistance_every_cycle = itmp;
	}
	else if(sscanf(token, "ntc=%lf,%lf", &ftmp, &ftmp2) == 2)
	{
		if(ftmp < 10 || ftmp > 65530.0 || ftmp2 < -100.0 || ftmp2 > 200.0)
		{
			printf("Warning: ignoring out-of-range ntc calibration pair (%f,%f)\n", ftmp, ftmp2);
		}
		else
		{
			if(num_t_cal_points >= MAX_T_CAL_POINTS-1)
			{
				printf("Warning: ignoring excess NTC calibration pair (%f, %f), max %u points allowed\n", ftmp, ftmp2, MAX_T_CAL_POINTS);
			}
			else
			{
				t_cal[num_t_cal_points][0] = ftmp;
				t_cal[num_t_cal_points][1] = ftmp2;
				num_t_cal_points++;
			}
		}
	}
	else
	{
		printf("Warning: ignoring unrecognized token %s\n", token);
	}

	return 0;
}

int parse_test_file(char* filename, test_t* params)
{
	char* buffer = malloc(10000);
	if(!buffer)
	{
		printf("Memory allocation error\n");
		return 1;
	}
	FILE* testfile = fopen(filename, "r");
	if(!testfile)
	{
		printf("Error opening file %s\n", filename);
		free(buffer);
		return 2;
	}

	while(fgets(buffer, 10000, testfile))
	{
		char* p = strtok(buffer, " \n\r");
		while(p != NULL)
		{
			if(parse_token(p, params))
			{
				free(buffer);
				fclose(testfile);
				return 3;
			}

			p = strtok(NULL, " \n\r");
		}
	}

	free(buffer);
	fclose(testfile);
	return 0;
}

void init_test(test_t* params)
{
	memset(params, 0, sizeof(*params));
}

int start_discharge(test_t* test)
{
	if(translate_configure_channel_hws(test, MODE_DISCHARGE) || set_test_mode(test, MODE_DISCHARGE))
		return -1;
	return 0;
}

int start_charge(test_t* test)
{
	if(translate_configure_channel_hws(test, MODE_CHARGE) || set_test_mode(test, MODE_CHARGE))
		return -1;
	return 0;
}

void update_test(test_t* test, int cur_time)
{
	if(test->kludgimus_maximus)
	{
//		printf("Warning: kludge in use. todo: fix HW not to give false CV information (set_current() -> also set i_override\n");
	}
	if(measure_hw(test, test->kludgimus_maximus) < 0)
	{
		go_fatal(test->fd, "measure_hw failed");
	}

	if(test->kludgimus_maximus) test->kludgimus_maximus--;
	if(update_measurement(test, 1) < 0)
	{
		go_fatal(test->fd, "update_measurement failed");
	}

//	if(cur_time == 7)
//		go_fatal(test->fd, "go_fatal test");

	if(test->cur_meas.temperature > test->temperature_stop && (test->cur_mode != MODE_OFF || test->next_mode != MODE_OFF))
	{
		printf("Info: Test %s overtemperature, stopping test.\n", test->name);
		fprintf(test->verbose_log, "Info: Test %s overtemperature, stopping test.\n", test->name);
		if(set_test_mode(test, MODE_OFF))
		{
			go_fatal(test->fd, "set_test_mode failed");
		}
		test->next_mode = MODE_OFF;
	}

	if(test->cur_meas.mode == MODE_OFF && test->cur_mode != MODE_OFF)
	{
		printf("Info: Cycle ended, setting test off.\n");
		if(test->cur_mode == MODE_CHARGE)
		{
			test->cooldown_start_time = cur_time;
			test->next_mode = MODE_DISCHARGE;
		}
		else if(test->cur_mode == MODE_DISCHARGE)
		{
			test->cooldown_start_time = cur_time;
			test->next_mode = MODE_CHARGE;
			test->cycle_cnt++;
		}
		set_test_mode(test, MODE_OFF);
	}


	if(test->resistance_on &&
	 (test->resistance_on_discharge_too || test->cur_mode==MODE_CHARGE)
	 && (test->cycle_cnt % test->resistance_every_cycle) == 0)
	{
		int tim = cur_time - test->cur_meas.start_time;
		int res_cycle_time = tim % test->resistance_interval;

		fprintf(test->verbose_log, "DBG: res_cycle = %d\n", res_cycle_time);
		// Allow 3 seconds to start resistance cycle measurement -- otherwise forget about it.
		if(res_cycle_time >= test->resistance_interval_offset && res_cycle_time <= test->resistance_interval_offset+2)
		{
			if(test->resistance_state == 0 && test->cur_meas.cccv == MODE_CC && (test->cur_mode == MODE_CHARGE || test->cur_mode == MODE_DISCHARGE))
			{
				fprintf(test->verbose_log, "DBG: resistance cycle -> 1\n");

				if(test->cur_mode == MODE_CHARGE)
					test_set_current(test, test->charge.current * test->resistance_first_pulse_current_mul);
				else if(test->cur_mode == MODE_DISCHARGE)
					test_set_current(test, -1 * test->discharge.current * test->resistance_first_pulse_current_mul);

				test->resistance_state = 1;
				test->resistance_last_v = test->cur_meas.voltage;
			}
		}

		if(res_cycle_time >= test->resistance_interval_offset + test->resistance_first_pulse_len)
		{
			if(test->resistance_state == 1)
			{
				fprintf(test->verbose_log, "DBG: resistance cycle -> 2\n");

				if(test->cur_mode == MODE_CHARGE)
					test_set_current(test, test->charge.current * test->resistance_second_pulse_current_mul);
				else if(test->cur_mode == MODE_DISCHARGE)
					test_set_current(test, -1 * test->discharge.current * test->resistance_second_pulse_current_mul);

				test->resistance_state = 2;
			}

		}

		if(res_cycle_time >= test->resistance_interval_offset + test->resistance_first_pulse_len + test->resistance_second_pulse_len)
		{
			if(test->resistance_state != 0)
			{
				fprintf(test->verbose_log, "DBG: resistance cycle -> 0\n");

				if(test->cur_mode == MODE_CHARGE)
				{
					fprintf(test->verbose_log, "DBG: V now: %f, last V: %f, dI: %f\n", test->cur_meas.voltage, test->resistance_last_v,
						(test->charge.current * (test->resistance_base_current_mul - test->resistance_second_pulse_current_mul)));
					test->cur_meas.resistance = (test->resistance_last_v - test->cur_meas.voltage) /
						(test->charge.current * (test->resistance_base_current_mul - test->resistance_second_pulse_current_mul));

					test_set_current(test, test->charge.current * test->resistance_base_current_mul);

				}
				else if(test->cur_mode == MODE_DISCHARGE)
				{
					fprintf(test->verbose_log, "DBG: V now: %f, last V: %f, dI: %f\n", test->cur_meas.voltage, test->resistance_last_v,
						(test->discharge.current * (test->resistance_base_current_mul - test->resistance_second_pulse_current_mul)));

					test->cur_meas.resistance = (test->cur_meas.voltage - test->resistance_last_v) /
						(test->discharge.current * (test->resistance_base_current_mul - test->resistance_second_pulse_current_mul));

					test_set_current(test, -1 * test->discharge.current * test->resistance_base_current_mul);
				}
				test->resistance_state = 0;
				test->kludgimus_maximus = 2;
			}
		}

		if(test->resistance_state && test->cur_meas.cccv != MODE_CC)
		{
			fprintf(test->verbose_log, "DBG: Test in CV - aborting resistance measurement.\n");
			if(test->cur_mode == MODE_CHARGE)
				test_set_current(test, test->charge.current * test->resistance_base_current_mul);
			else if(test->cur_mode == MODE_DISCHARGE)
				test_set_current(test, -1 * test->discharge.current * test->resistance_base_current_mul);
			test->resistance_state = 0;
		}
	}  // end if resistance measurement
	else if(test->cur_mode == MODE_DISCHARGE && test->discharge.const_power_mode && cur_time%10 == 5)
	{
		double new_current = test->discharge.power / test->cur_meas.voltage;
		if(new_current / test->num_channels > (HW_MAX_CURRENT-50)/1000.0)
		{
			go_fatal(test->fd, "constant power overcurrent");
		}
		printf("dbg: Const power: Setting test current to %.2f A * %.3f V = %.2f W\n", new_current, test->cur_meas.voltage, new_current*test->cur_meas.voltage);
		test_set_current(test, -1 * new_current);

	}

	printf("test=%s cycle=%u ", test->name, test->cycle_cnt);
	print_measurement(&test->cur_meas, cur_time - test->cur_meas.start_time);
	printf("\n");
	log_measurement(&test->cur_meas, test, cur_time - test->cur_meas.start_time);

	clear_hw_measurements(test);
	test->cur_meas.resistance = 0.0;

	if(test->cur_mode == MODE_OFF && test->next_mode == MODE_DISCHARGE)
	{
		printf("Info: Starting discharge in %d seconds...    \r", test->cooldown_start_time + test->postcharge_cooldown - cur_time); fflush(stdout);
		if(cur_time >= test->cooldown_start_time + test->postcharge_cooldown)
		{
			printf("\n");
			test->cur_meas.start_time = cur_time;
			test->cur_meas.cumul_ah = 0.0;
			test->cur_meas.cumul_wh = 0.0;

			if(start_discharge(test) < 0)
			{
				go_fatal(test->fd, "start_discharge failed");
			}
			sleep(1);
		}
	}
	else if(test->cur_mode == MODE_OFF && test->next_mode == MODE_CHARGE)
	{
		printf("Info: Starting charge in %d seconds...    \r", test->cooldown_start_time + test->postdischarge_cooldown - cur_time); fflush(stdout);
		if(cur_time >= test->cooldown_start_time + test->postdischarge_cooldown)
		{
			printf("\n");
			test->cur_meas.start_time = cur_time;
			test->cur_meas.cumul_ah = 0.0;
			test->cur_meas.cumul_wh = 0.0;

			if(start_charge(test) < 0)
			{
				go_fatal(test->fd, "start_charge failed");
			}
			sleep(1);
		}
	}
}

int prepare_test(test_t* test)
{
	char buf[200];
	int ret;
	int ch;

	test->cur_mode = MODE_OFF;
	test->next_mode = test->start_mode;
	test->cooldown_start_time = -999999; // this forces the test to start
	if((test->fd = open_device(test->device_name)) < 0)
	{
		printf("Error: open_device returned %d\n", test->fd);
		return -1;
	}

	for(ch = 0; ch < test->num_channels; ch++)
	{
		uart_flush(test->fd);
		sprintf(buf, "@%u:OFF;", test->channels[ch]);
		comm_send(test->fd, buf);
		if((ret = comm_expect(test->fd, "OFF OK")))
		{
			printf("Test preparation failed; comm_expect for first OFF message returned %d\n", ret);
			close_device(test->fd);
			return -2;
		}
//		usleep(200000); // todo: verify that this indeed is no longer necessary
	}


	return 0;
}

void run(int num_tests, test_t* tests)
{
	int pc_start_time = (int)(time(0));
	int prev_time = -1;

	while(1)
	{
		int cur_time;

		do
		{
			usleep(500);
			cur_time = (int)(time(0))-pc_start_time;
		}
		while(cur_time == prev_time);

		prev_time = cur_time;

		int t;
		for(t=0; t<num_tests; t++)
		{
			update_test(&tests[t], cur_time);
		}
		printf("\n");
	}

}



int main(int argc, char** argv)
{
	int num_tests = argc-1;
	if(num_tests < 1)
	{
		printf("Usage: kakkor <test1> [test2] [test3] ...\n");
		return 1;
	}

	test_t* tests = malloc(num_tests*sizeof(test_t));
	int t;
	for(t=0; t < num_tests; t++)
	{
		init_test(&tests[t]);
		tests[t].name = malloc(strlen(argv[t+1])+1);
		strcpy(tests[t].name, argv[t+1]);
		if(parse_test_file("defaults", &tests[t]))
		{
			free(tests);
			return 1;
		}
		if(parse_test_file(argv[t+1], &tests[t]))
		{
			free(tests);
			return 1;
		}
	}

	for(t = 0; t < num_tests; t++)
	{
		if(check_params(&tests[t]) || translate_settings(&tests[t]) || prepare_test(&tests[t]) || start_log(&tests[t]))
		{
			free(tests);
			return 1;
		}
		print_params(&tests[t]);
	}

	run(num_tests, tests);

	return 0;
}
