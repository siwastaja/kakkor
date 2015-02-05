#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

typedef enum {MODE_OFF = 0, MODE_CHARGE, MODE_DISCHARGE, MODE_CHA_DSCH, MODE_DSCH_CHA} mode_t;

const char* mode_names[5] = {"MODE_OFF", "MODE_CHARGE", "MODE_DISCHARGE", "MODE_CHARGE_TO_DISCHARGE", "MODE_DISCHARGE_TO_CHARGE"};
const char* short_mode_names[5] = {"OFF", "CHA", "DSCH", "CHA2DSCH", "DSCH2CHA"};

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
	hw_measurement_t hwmeas[MAX_PARALLEL_CHANNELS];
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
	int num_channels;
	int channels[MAX_PARALLEL_CHANNELS];

	// List of channels to be averaged to the final voltage reading.
	// If no sense wires are used, best to average all channels in parallel
	// If sense wires are used, choose all channels that use them (typically 1 for lazy installation).
	int num_voltchannels;
	int voltchannels[MAX_PARALLEL_CHANNELS];

	base_settings_t charge;
	base_settings_t discharge;
	hw_base_settings_t hw_charge;
	hw_base_settings_t hw_discharge;

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
	fprintf(t->log, "%u%s%s%s%s%s%.3f%s%.2f%s%.1f%s%.3f%s%.3f%s%.2f",
		time, delim, short_mode_names[m->mode], delim, short_cccv_names[m->cccv], delim,
		m->voltage, delim, m->current, delim, m->temperature, delim, m->cumul_ah, delim, m->cumul_wh, delim, m->resistance);
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

void uart_flush()
{
	return;
}

void comm_send(char* buf)
{
	printf("comm_send: |%s|\n", buf);
}

int comm_expect(char* buf)
{
	return 0;
}


void aread(int fd, char* buf, int n)
{
	static int jutska = 0;
	switch(jutska)
	{
		case 0:
		strcpy(buf, "kakkenpis sen vir\ntsen:629"); break;
		case 1:
		strcpy(buf, "johannes;@1:VMEAS=123 IMEAS=45"); break;
		case 2:
		strcpy(buf, "6 TMEAS=789"); break;
		case 3:
		strcpy(buf, ";asfgdashjughaeghu;gta"); break;
		case 4:
		strcpy(buf, "safsa;@MUOVIKUKKA;;@kakkapissa"); break;
		case 5:
		strcpy(buf, ";;@kakka;;"); break;
		case 6:
		strcpy(buf, "@viela yksi"); break;
		case 7:
		strcpy(buf, ";"); break;
		default:
		buf[0] = 0;
		break;
	}

	jutska++;
}


//#define EXPECT_HEADER ';'
//#define EXPECT_FOOTER ';'

#define COMM_SEPARATOR ';'
#define MAX_READBUF_LEN 200

int read_reply(int fd, char* outbuf, int maxbytes, int flush)
{
	static char old_readbuf[MAX_READBUF_LEN];
	char readbuf[MAX_READBUF_LEN];
	char* p_readbuf;

	if(flush)
	{
		uart_flush();
	}

	aread(fd, readbuf, MAX_READBUF_LEN-1);
	p_readbuf = readbuf;

	while(1)
	{
		p_readbuf++;
	}
}

/*
int read_reply(int fd, char *outbuf, int maxbytes, int flush)
{
	static char old_readbuf[200];
	char readbuf[200];
	char* p_readbuf;

	if(flush)
	{
		uart_flush();
		old_readbuf[0] = 0;
	}

	while(1)
	{
		// Read as long as we get our expected header.
		if(old_readbuf[0] != 0)
		{
			// We have surpluss stuff from the previous read, process that first
			strcpy(readbuf, old_readbuf);
			printf("oldbuf: %s\n", readbuf);
		}
		else
		{
			aread(fd, readbuf, 199);
			printf("read1: %s\n", readbuf);
		}
		if(readbuf[0] == 0)
			return 5;
		if((p_readbuf = strchr(readbuf, EXPECT_HEADER)))
		{
			p_readbuf++;
			// Got the header; read as long as we find the footer.
			while(1)
			{
				char* p_footer;
				if((p_footer = strchr(p_readbuf, EXPECT_FOOTER)))
				{
					// Found the footer.
					int size = p_footer - p_readbuf;
					if(size < 0)
						return 1;
					if(size >= maxbytes)
						return 2;
					size++;
					if(size > maxbytes)
						return 4;
					*p_footer = 0;
					strcpy(outbuf, p_readbuf);
					// Copy any excess data after the footer for the next round
					// p_footer points to the footer, which we
					// replaced with 0. Now jump over the replaced footer
					// and copy whatever there
					// is. If nothing, there is the original terminating zero.
					p_footer++;
					strcpy(old_readbuf, p_footer);
					return 0;
				}

				// Didn't found the footer yet, copy what we have
				// and read more.
				int size = strlen(p_readbuf);
				if(size >= maxbytes)
					return 3;
				maxbytes -= size;
				printf("strcpy %s\n", p_readbuf);
				strcpy(outbuf, p_readbuf);
				outbuf+=size;
				usleep(1000);
				aread(fd, readbuf, 199);
				printf("read2: %s\n", readbuf);
				p_readbuf = readbuf;
			}

		}

		if(old_readbuf[0] == 0)
			usleep(1000); // we did a read and did not get everything - wait for more data.

		old_readbuf[0] = 0;

	}

	return 0;
}

*/

int measure_hw(int fd, test_t* test)
{
	char buf[200];
	int i;

	for(i = 0; i < test->num_channels; i++)
	{
		uart_flush();
		sprintf(buf, ";@%u:MEAS;", test->channels[i]);
		comm_send(buf);


	}

	return 0;
}

int configure_hw(int fd, test_t* params, mode_t mode)
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
		uart_flush();
		sprintf(buf, ";@%u:OFF;", params->channels[i]);
		comm_send(buf);
		if(comm_expect("OFF OK"))
			return 1;
		sprintf(buf, ";@%u:SETI %d;", params->channels[i], settings->current);
		comm_send(buf);
		if(comm_expect("SETI OK"))
			return 1;
		sprintf(buf, ";@%u:SETV %d;", params->channels[i], settings->voltage);
		comm_send(buf);
		if(comm_expect("SETV OK"))
			return 1;
		sprintf(buf, ";@%u:SETISTOP %d;", params->channels[i], settings->stop_current);
		comm_send(buf);
		if(comm_expect("SETISTOP OK"))
			return 1;
		sprintf(buf, ";@%u:SETVSTOP %d;", params->channels[i], settings->stop_voltage);
		comm_send(buf);
		if(comm_expect("SETVSTOP OK"))
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
	params->name = malloc(strlen(filename)+1);
	strcpy(params->name, filename);

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

void update_test(test_t* test, int time)
{
	measure_hw(123, test);
	if(test->cur_mode == MODE_CHA_DSCH)
	{
		if(time >= test->postcharge_cooldown_start_time + test->postcharge_cooldown)
		{
			start_discharge(test);
		}
	}
	else if(test->cur_mode == MODE_DSCH_CHA)
	{
		if(time >= test->postdischarge_cooldown_start_time + test->postdischarge_cooldown)
		{
			start_charge(test);
		}
	}
}

void run()
{
	int num_tests = 1;
	test_t* tests = malloc(num_tests*sizeof(test_t));
	init_test(&tests[0]);
	parse_test_file("test1", &tests[0]);
	check_params(&tests[0]);
	translate_settings(&tests[0]);
	print_params(&tests[0]);

	int pc_start_time = (int)(time(0));

	while(1)
	{
		int cur_time = (int)(time(0))-pc_start_time;
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
	configure_hw(0, &test1, MODE_CHARGE);
	configure_hw(0, &test1, MODE_DISCHARGE);
*/
	int i;
	for(i = 0; i < 10; i++)
	{
		char testbuf[1000];
		int ret = read_reply(0, testbuf, 999, 0);
		printf("%u: |RETURN %d||%s\n", i, ret, testbuf);
	}

	return 0;
}
