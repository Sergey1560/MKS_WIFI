#ifndef MAIN_H
#define MAIN_H

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include "RepRapWebServer.h"
#include "MksHTTPUpdateServer.h"
#include <EEPROM.h>
#include <FS.h>
#include <ESP8266HTTPClient.h>
#include "PooledStrings.cpp"
#include <WiFiUdp.h>
#include "Config.h"
#include "gcode.h"
#include "user_interface.h"     // for struct rst_info

#define BUTTON_PIN -1
#define MAX_WIFI_FAIL 50
#define MAX_LOGGED_IN_CLIENTS 3

#define BAK_ADDRESS_WIFI_SSID			0
#define BAK_ADDRESS_WIFI_KEY			(BAK_ADDRESS_WIFI_SSID + 32)
#define BAK_ADDRESS_WEB_HOST			(BAK_ADDRESS_WIFI_KEY+64)
#define BAK_ADDRESS_WIFI_MODE		(BAK_ADDRESS_WEB_HOST+64)
#define BAK_ADDRESS_WIFI_VALID		(BAK_ADDRESS_WIFI_MODE + 16)
#define BAK_ADDRESS_MODULE_ID		(BAK_ADDRESS_WIFI_VALID + 16)
#define BAK_ADDRESS_RESERVE1		(BAK_ADDRESS_MODULE_ID + 32)
#define BAK_ADDRESS_RESERVE2		(BAK_ADDRESS_RESERVE1 + 16)
#define BAK_ADDRESS_RESERVE3		(BAK_ADDRESS_RESERVE2 + 96)
#define BAK_ADDRESS_RESERVE4		(BAK_ADDRESS_RESERVE3 + 16)
#define BAK_ADDRESS_MANUAL_IP_FLAG	(BAK_ADDRESS_RESERVE4 + 1)
#define BAK_ADDRESS_MANUAL_IP			(BAK_ADDRESS_MANUAL_IP_FLAG + 1)
#define BAK_ADDRESS_MANUAL_MASK		(BAK_ADDRESS_MANUAL_IP + 4)
#define BAK_ADDRESS_MANUAL_GATEWAY	(BAK_ADDRESS_MANUAL_MASK + 4)
#define BAK_ADDRESS_MANUAL_DNS		(BAK_ADDRESS_MANUAL_GATEWAY + 4)

#define LIST_MIN_LEN_SAVE_FILE	100
#define LIST_MAX_LEN_SAVE_FILE  (1024 * 100)

#define FILE_DOWNLOAD_PORT	11188

#define MAX_SRV_CLIENTS 	1
#define QUEUE_MAX_NUM	 10

#define UDP_PORT	8989

#define TCP_FRAG_LEN	1400

#ifdef SHOW_PASSWORDS
# define PASSWORD_INPUT_TYPE  "\"text\""
#else
# define PASSWORD_INPUT_TYPE  "\"password\""
#endif

#define FILE_FIFO_SIZE	(4096)
#define BUF_INC_POINTER(p)	((p + 1 == FILE_FIFO_SIZE) ? 0:(p + 1))


#define FILE_BLOCK_SIZE	(1024 - 5 - 4)


#define UART_PROTCL_HEAD_OFFSET		0
#define UART_PROTCL_TYPE_OFFSET		1
#define UART_PROTCL_DATALEN_OFFSET	2
#define UART_PROTCL_DATA_OFFSET		4

#define UART_PROTCL_HEAD	(char)0xa5
#define UART_PROTCL_TAIL	(char)0xfc

#define UART_PROTCL_TYPE_NET			(char)0x0
#define UART_PROTCL_TYPE_GCODE		(char)0x1
#define UART_PROTCL_TYPE_FIRST			(char)0x2
#define UART_PROTCL_TYPE_FRAGMENT		(char)0x3
#define UART_PROTCL_TYPE_HOT_PORT		(char)0x4
#define UART_PROTCL_TYPE_STATIC_IP		(char)0x5

/*******************************************************************
    receive data from stm32 handler

********************************************************************/
#define UART_RX_BUFFER_SIZE    1024

#define ESP_PROTOC_HEAD	(uint8_t)0xa5
#define ESP_PROTOC_TAIL		(uint8_t)0xfc

#define ESP_TYPE_NET			(uint8_t)0x0
#define ESP_TYPE_PRINTER		(uint8_t)0x1
#define ESP_TYPE_TRANSFER		(uint8_t)0x2
#define ESP_TYPE_EXCEPTION		(uint8_t)0x3
#define ESP_TYPE_CLOUD			(uint8_t)0x4
#define ESP_TYPE_UNBIND		(uint8_t)0x5
#define ESP_TYPE_WID			(uint8_t)0x6
#define ESP_TYPE_SCAN_WIFI		(uint8_t)0x7
#define ESP_TYPE_MANUAL_IP		(uint8_t)0x8
#define ESP_TYPE_WIFI_CTRL		(uint8_t)0x9


enum class OperatingState
{
    Unknown = 0,
    Client = 1,
    AccessPoint = 2    
};


struct QUEUE{
	char buf[QUEUE_MAX_NUM][100];
	int rd_index;
	int wt_index;
};

typedef enum{
	TRANSFER_IDLE,
	TRANSFER_BEGIN,
	TRANSFER_GET_FILE,
	TRANSFER_READY,	
	TRANSFER_FRAGMENT
} TRANS_STATE;

typedef enum{
	CLOUD_NOT_CONNECT,
	CLOUD_IDLE,
	CLOUD_DOWNLOADING,
	CLOUD_DOWN_WAIT_M3,
	CLOUD_DOWNLOAD_FINISH,
	CLOUD_WAIT_PRINT,
	CLOUD_PRINTING,
	CLOUD_GET_FILE,
} CLOUD_STATE;

typedef struct{
	uint8_t head; //0xa5
	uint8_t type; 
	uint16_t dataLen; 
	uint8_t *data;
	uint8_t tail; // 0xfc
} ESP_PROTOC_FRAME;


void do_transfer(void);
int package_gcode(char *dataField, boolean important);
void cloud_down_file();
int package_gcode(String gcodeStr, boolean important);
int package_file_first(char *fileName);
int package_file_fragment(uint8_t *dataField, uint32_t fragLen, int32_t fragment);

void esp_data_parser(char *cmdRxBuf, int len);
String fileUrlEncode(String str);
String fileUrlEncode(char *array);

void cloud_handler();
void cloud_get_file_list();

void fsHandler();
void handleGcode();
void handleRrUpload();
void  handleUpload();

void urldecode(String &input);
void urlencode(String &input);
void StartAccessPoint();
void SendInfoToSam();
bool TryToConnect();
void onWifiConfig();

void cloud_down_file(const char *url);

int package_static_ip_info(void);
int package_net_para(void);

int32_t charAtArray(const uint8_t *_array, uint32_t _arrayLen, uint8_t _char);
int cut_msg_head(uint8_t *msg, uint16_t msgLen, uint16_t cutLen);
void net_msg_handle(uint8_t * msg, uint16_t msgLen);
void cloud_msg_handle(uint8_t * msg, uint16_t msgLen);
void scan_wifi_msg_handle(void);

void extract_file_item(File dataFile, String fileStr);
void extract_file_item_cloud(File dataFile, String fileStr);
int get_printer_reply(void);

#endif