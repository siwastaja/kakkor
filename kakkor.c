#include <stdio.h>
#include <string.h>
#include <malloc.h>

typedef enum {MODE_OFF = 0, MODE_CHARGE, MODE_DISCHARGE} mode_t;
typedef enum {STOP_MODE_UNDEFINED = 0, STOP_MODE_CURRENT, STOP_MODE_VOLTAGE} stop_mode_t;

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
	char* name;
	int num_channels;
	int channels[MAX_PARALLEL_CHANNELS];
	base_settings_t charge;
	base_settings_t discharge;
	hw_base_settings_t hw_charge;
	hw_base_settings_t hw_discharge;
} test_t;

void print_params(test_t* params)
{
	int i;
	printf("%u parallel channels: ", params->num_channels);
	for(i = 0; i < params->num_channels; i++)
		printf("%u  ", params->channels[i]);
	printf("\nCHARGE: current=%.3f   voltage=%.3f   stop_mode=%u   stop_current=%.3f   stop_voltage=%.3f\n",
		params->charge.current, params->charge.voltage, params->charge.stop_mode, params->charge.stop_current, params->charge.stop_voltage);
	printf("DISCHARGE: current=%.3f   voltage=%.3f   stop_mode=%u   stop_current=%.3f   stop_voltage=%.3f\n",
		params->discharge.current, params->discharge.voltage, params->discharge.stop_mode, params->discharge.stop_current, params->discharge.stop_voltage);

	printf("HW CHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_charge.current, params->hw_charge.stop_current, params->hw_charge.voltage, params->hw_charge.stop_voltage);

	printf("HW DISCHARGE: current=%d   stopcurrent=%d   voltage=%d   stopvoltage=%d\n",
		params->hw_discharge.current, params->hw_discharge.stop_current, params->hw_discharge.voltage, params->hw_discharge.stop_voltage);

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

int main()
{
	test_t test1;
	memset(&test1, 0, sizeof(test1));
	parse_test_file("test1", &test1);
	check_params(&test1);
	translate_settings(&test1);
	print_params(&test1);
	configure_hw(0, &test1, MODE_CHARGE);
	configure_hw(0, &test1, MODE_DISCHARGE);

	return 0;
}
