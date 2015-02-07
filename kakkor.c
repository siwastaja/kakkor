#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#include "comm_uart.h"

typedef enum {MODE_UNDEFINED = 0, MODE_OFF, MODE_CHARGE, MODE_DISCHARGE, MODE_CHA_DSCH, MODE_DSCH_CHA} mode_t;

const char* mode_names[6] = {"MODE_UNDEFINED", "MODE_OFF", "MODE_CHARGE", "MODE_DISCHARGE", "MODE_CHARGE_TO_DISCHARGE", "MODE_DISCHARGE_TO_CHARGE"};
const char* short_mode_names[6] = {"UNDEF", "OFF", "CHA", "DSCH", "CHA2DSCH", "DSCH2CHA"};

typedef enum {STOP_MODE_UNDEFINED = 0, STOP_MODE_CURRENT, STOP_MODE_VOLTAGE} stop_mode_t;

const char* stop_mode_names[3] = {"UNDEFINED", "CURRENT STOP", "VOLTAGE STOP"};
const char* short_stop_mode_names[3] = {"UNDEF", "ISTOP", "VSTOP"};

typedef enum {CCCV_UNDEFINED, MODE_CC, MODE_CV} cccv_t;
const char* short_cccv_names[3] = {"UNDEF", "CC", "CV"};


const char* delim = ";";

#define MAX_PARALLEL_CHANNELS 32
#define MIN_ID 0
#define MAX_ID 255

typedef struct
{
	double current;
	double voltage;
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
	mode_t mode;
	cccv_t cccv;
} hw_measurement_t;

typedef struct
{
	int num_hw_measurements;
	hw_measurement_t hw_meas[MAX_PARALLEL_CHANNELS];
	double voltage;
	double current;
	double temperature;
	mode_t mode;
	cccv_t cccv;
	double cumul_ah;
	double cumul_wh;
	double resistance;
} measurement_t;

typedef struct
{
	char* name;
	char* device_name;
	int fd;
	int num_channels;
	int channels[MAX_PARALLEL_CHANNELS];

	// List of channels to be averaged to the final voltage reading.
	// If no sense wires are used, best to average all channels in parallel
	// If sense wires are used, choose all channels that use them (typically 1 for lazy installation).
	// This same list is used for temperature sensors, currently...
	int num_voltchannels;
	int voltchannels[MAX_PARALLEL_CHANNELS];

	base_settings_t charge;
	base_settings_t discharge;
	hw_base_settings_t hw_charge;
	hw_base_settings_t hw_discharge;

	measurement_t cur_meas;

	mode_t start_mode;
	int start_time;
	int postcharge_cooldown;
	int postdischarge_cooldown;
	int postcharge_cooldown_start_time;
	int postdischarge_cooldown_start_time;

	int cycle_cnt;
	mode_t cur_mode;
	FILE* log;
	FILE* verbose_log;

} test_t;

void log_measurement(measurement_t* m, test_t* t, int time)
{
	fprintf(t->log, "%u%s%s%s%s%s%.3f%s%.2f%s%.1f%s%.4f%s%.3f%s%.2f",
		time, delim, short_mode_names[m->mode], delim, short_cccv_names[m->cccv], delim,
		m->voltage, delim, m->current, delim, m->temperature, delim, m->cumul_ah, delim, m->cumul_wh, delim, m->resistance);
}

void print_measurement(measurement_t* m, int time)
{
	printf("time=%u %s %s V=%.3f I=%.2f T=%.1f Ah=%.4f Wh=%.3f R=%.2f",
		time, short_mode_names[m->mode], short_cccv_names[m->cccv],
		m->voltage, m->current, m->temperature, m->cumul_ah, m->cumul_wh, m->resistance);
}


#define HW_MIN_VOLTAGE 0
#define HW_MAX_VOLTAGE 7000
#define HW_MIN_CURRENT -32000
#define HW_MAX_CURRENT 32000
#define HW_MIN_TEMPERATURE 0
#define HW_MAX_TEMPERATURE 65535


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
	if(strstr(str, "MEAS") != str)
		return -1;

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
		meas->cccv = MODE_CV;

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
		if(meas->current < HW_MIN_TEMPERATURE || meas->current > HW_MAX_TEMPERATURE)
			return -9;
	}

	return 0;
}

void print_params(test_t* params)
{
	int i;
	printf("%u parallel channels: ", params->num_channels);
	for(i = 0; i < params->num_channels; i++)
		printf("%u  ", params->channels[i]);
	printf("\nCHARGE: current=%.3f   voltage=%.3f   stop_mode=%s   stop_current=%.3f   stop_voltage=%.3f\n",
		params->charge.current, params->charge.voltage, short_stop_mode_names[params->charge.stop_mode], params->charge.stop_current, params->charge.stop_voltage);
	printf("DISCHARGE: current=%.3f   voltage=%.3f   stop_mode=%s   stop_current=%.3f   stop_voltage=%.3f\n",
		params->discharge.current, params->discharge.voltage, short_stop_mode_names[params->discharge.stop_mode], params->discharge.stop_current, params->discharge.stop_voltage);

	printf("HW CHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_charge.current, params->hw_charge.stop_current, params->hw_charge.voltage, params->hw_charge.stop_voltage);

	printf("HW DISCHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_discharge.current, params->hw_discharge.stop_current, params->hw_discharge.voltage, params->hw_discharge.stop_voltage);

	printf("cycling_start=%s   postcharge_cooldown=%u sec   postdischarge_cooldown=%u sec\n",
		mode_names[params->start_mode], params->postcharge_cooldown, params->postdischarge_cooldown);
}

int update_measurement(test_t* test, double elapsed_seconds)
{
	int ch;

	double voltage_sum = 0.0;
	double temperature_sum = 0.0;
	for(ch = 0; ch < test->num_voltchannels; ch++)
	{
		int idx = get_channel_idx(test, test->voltchannels[ch]);
		voltage_sum += (double)test->cur_meas.hw_meas[idx].voltage / 1000.0;
		temperature_sum += (double)test->cur_meas.hw_meas[idx].temperature / 65535.0;
	}
	voltage_sum /= (double)test->num_voltchannels;
	test->cur_meas.voltage = voltage_sum;
	test->cur_meas.temperature = temperature_sum / (double)test->num_voltchannels;

	double current_sum = 0;
	for(ch = 0; ch < test->num_channels; ch++)
	{
		current_sum += (double)test->cur_meas.hw_meas[ch].current / 1000.0;
	}
	current_sum /= (double)test->num_channels;

	test->cur_meas.current = current_sum;
	test->cur_meas.cumul_ah += current_sum * elapsed_seconds / 3600.0;
	test->cur_meas.cumul_wh += current_sum * voltage_sum * elapsed_seconds / 3600.0;

	return 0;
}

int measure_hw(test_t* test)
{
	char buf[200];
	char* p_buf;
	int i;
	int ret;

	uart_flush(test->fd);
	for(i = 0; i < test->num_channels; i++)
	{
		sprintf(buf, ";@%u:MEAS;", test->channels[i]);
		comm_send(test->fd, buf);
		if(read_reply(test->fd, buf, 200))
		{
			printf("Error getting measurement data\n");
			return -1;
		}
		printf("measure_hw: got reply: %s\n", buf);

		int id = -1;
		int n = 0;
		if(sscanf(buf, "%u:%n", &id, &n) != 1)
		{
			printf("ID sscanf error\n");
			return -1;
		}
		if(id != test->channels[i])
		{
			printf("Error: Got reply from wrong id: expected %d, got %d\n",
				test->channels[i], id);
			return -1;
		}
		p_buf = buf + n;

		hw_measurement_t meas;
		if((ret = parse_hw_measurement(&meas, p_buf)))
		{
			printf("Error: parse_hw_measurement returned %d\n", ret);
			return -1;
		}

		if((ret = add_measurement(test, id, &meas)))
		{
			printf("Error: add_measurement returned %d\n", ret);
			return -1;
		}

	}

	if(test->cur_meas.num_hw_measurements != test->num_channels)
	{
		printf("Error: didn't get measurements from all channels\n");
		return -1;
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
		return 555;

	for(i = 0; i < params->num_channels; i++)
	{
		uart_flush(params->fd);
		sprintf(buf, ";@%u:OFF;", params->channels[i]);
		comm_send(params->fd, buf);
		if(comm_expect(params->fd, "OFF OK"))
			return 1;
		sprintf(buf, ";@%u:SETI %d;", params->channels[i], settings->current);
		comm_send(params->fd, buf);
		if(comm_expect(params->fd, "SETI OK"))
			return 1;
		sprintf(buf, ";@%u:SETV %d;", params->channels[i], settings->voltage);
		comm_send(params->fd, buf);
		if(comm_expect(params->fd, "SETV OK"))
			return 1;
		sprintf(buf, ";@%u:SETISTOP %d;", params->channels[i], settings->stop_current);
		comm_send(params->fd, buf);
		if(comm_expect(params->fd, "SETISTOP OK"))
			return 1;
		sprintf(buf, ";@%u:SETVSTOP %d;", params->channels[i], settings->stop_voltage);
		comm_send(params->fd, buf);
		if(comm_expect(params->fd, "SETVSTOP OK"))
			return 1;
	}

	return 0;
}

int translate_settings(test_t* params)
{
	params->hw_charge.current = (int)(params->charge.current*1000.0/params->num_channels);
	if(params->charge.stop_mode == STOP_MODE_CURRENT)
	{
		params->hw_charge.stop_current = (int)(params->charge.stop_current*1000.0/params->num_channels);
		params->hw_charge.voltage = (int)(params->charge.voltage*1000.0);
		// In current stop mode, stop voltage will be set so that it's never met - HW does not use
		// "stop modes" at all. Stop voltage will be used as a kind of safety measure.
		params->hw_charge.stop_voltage = (int)((params->charge.voltage+0.1)*1000.0);
	}
	else if(params->charge.stop_mode == STOP_MODE_VOLTAGE)
	{
		params->hw_charge.stop_voltage = (int)(params->charge.stop_voltage*1000.0);
		// Same here - stop mode is achieved by having stop voltage lower than CV voltage.
		params->hw_charge.voltage = (int)((params->charge.stop_voltage+0.1)*1000.0);
		params->hw_charge.stop_current = 1; // stop current will never be met.
	}
	else
	{
		printf("ERROR: translate_settings: illegal stop_mode\n");
		return 1;
	}

	params->hw_discharge.current = -1*((int)(params->discharge.current*1000.0/params->num_channels));
	if(params->discharge.stop_mode == STOP_MODE_CURRENT)
	{
		params->hw_discharge.stop_current = -1*((int)(params->discharge.stop_current*1000.0/params->num_channels));
		params->hw_discharge.voltage = (int)(params->discharge.voltage*1000.0);
		params->hw_discharge.stop_voltage = (int)((params->discharge.voltage-0.1)*1000.0);
	}
	else if(params->discharge.stop_mode == STOP_MODE_VOLTAGE)
	{
		params->hw_discharge.stop_voltage = (int)(params->discharge.stop_voltage*1000.0);
		params->hw_discharge.voltage = (int)((params->discharge.stop_voltage-0.1)*1000.0);
		params->hw_discharge.stop_current = -1;
	}
	else
	{
		printf("ERROR: translate_settings: illegal stop_mode\n");
		return 1;
	}

	return 0;

}

#define MIN_CURRENT 0.0
#define MAX_CURRENT 1000.0
int check_base_settings(char* name, base_settings_t* params)
{
	if(params->current == 0.0)
	{
		printf("ERROR: %s: Current missing\n", name);
		return 1;
	}

	if(params->current < MIN_CURRENT || params->current > MAX_CURRENT)
	{
		printf("ERROR: %s: Illegal current: %.3f\n", name, params->current);
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

	return 0;
}

int check_params(test_t* params)
{
	int i;

	if(params->num_channels < 1 || params->num_channels > MAX_PARALLEL_CHANNELS)
	{
		printf("Illegal number of parallel channels: %u\n", params->num_channels);
		return 1;
	}

	for(i = 0; i < params->num_channels; i++)
	{
		if(params->channels[i] < MIN_ID || params->channels[i] > MAX_ID)
		{
			printf("Illegal channel ID: %u\n", params->channels[i]);
			return 1;
		}
	}

	if(check_base_settings("Charge", &params->charge))
		return 1;
	if(check_base_settings("Discharge", &params->discharge))
		return 1;

	return 0;
}


int parse_token(char* token, test_t* params)
{
	static mode_t param_state = MODE_OFF;
	int n;
	double ftmp;
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
				printf("Too many parallel channels\n");
				return 1;
			}
		}
		return 0;
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
				return 3;
			}

			p = strtok(NULL, " \n\r");
		}
	}

	return 0;
}

void init_test(test_t* params)
{
	memset(params, 0, sizeof(*params));
}

void start_discharge(test_t* test)
{
	test->cur_mode = MODE_DISCHARGE;
}

void start_charge(test_t* test)
{
	test->cur_mode = MODE_CHARGE;
}

void update_test(test_t* test, int cur_time)
{
	measure_hw(test);
	update_measurement(test, 1);
	print_measurement(&test->cur_meas, cur_time);

	if(test->cur_mode == MODE_CHA_DSCH)
	{
		if(cur_time >= test->postcharge_cooldown_start_time + test->postcharge_cooldown)
		{
			start_discharge(test);
		}
	}
	else if(test->cur_mode == MODE_DSCH_CHA)
	{
		if(cur_time >= test->postdischarge_cooldown_start_time + test->postdischarge_cooldown)
		{
			start_charge(test);
		}
	}
}

int prepare_test(test_t* test)
{
	int ret;
	if((test->fd = open_device(test->device_name)) < 0)
	{
		printf("Error: open_device returned %d\n", test->fd);
		return -1;
	}

	uart_flush(test->fd);
	comm_send(test->fd, ";@1:OFF;");
	if((ret = comm_expect(test->fd, "OFF OK")))
	{
		printf("Test preparation failed; comm_expect for first OFF message returned %d\n", ret);
		close_device(test->fd);
		return -2;
	}


	return 0;
}

void run()
{
	int num_tests = 1;
	test_t* tests = malloc(num_tests*sizeof(test_t));
	init_test(&tests[0]);
	parse_test_file("defaults", &tests[0]);
	parse_test_file("test1", &tests[0]);

	check_params(&tests[0]);
	translate_settings(&tests[0]);
	print_params(&tests[0]);
	if(prepare_test(&tests[0]))
	{
		printf("Stopped.\n");
		return;
	}

	int pc_start_time = (int)(time(0));
	int prev_time = -1;

	while(1)
	{
		int cur_time;

		while(cur_time != prev_time)
		{
			usleep(500);
			prev_time = cur_time;
			cur_time = (int)(time(0))-pc_start_time;
		}

		int t;
		for(t=0; t<num_tests; t++)
		{
			update_test(&tests[0], cur_time);
		}
	}

}



int main()
{
/*	test_t test1;
	init_test(&test1);
	parse_test_file("test1", &test1);
	check_params(&test1);
	translate_settings(&test1);
	print_params(&test1);
	configure_hw(&test1, MODE_CHARGE);
	configure_hw(&test1, MODE_DISCHARGE);
*/

	run();

	return 0;
}
