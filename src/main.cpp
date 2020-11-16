#include "main.h"


const char* firmwareVersion = "C1.0.4_201109_beta";
char M3_TYPE = TFT28;

boolean GET_VERSION_OK = false;

char wifi_mode[] = "wifi_mode_sta";
char moduleId[21] = {0};
char softApName[32] = "MKSWIFI";
char softApKey[64] = {0};
uint8_t manual_valid = 0xff; 
uint32_t ip_static, subnet_static, gateway_staic, dns_static;
char ssid[32], pass[64], webhostname[64];
uint8_t loggedInClientsNum = 0;
MksHTTPUpdateServer httpUpdater;
char cloud_host[96] = "baizhongyun.cn";
int cloud_port = 12345;
boolean cloud_enable_flag = false;
int cloud_link_state = 0; 
RepRapWebServer server(80);
WiFiServer tcp(8080);
WiFiClient cloud_client;
String wifiConfigHtml;
volatile bool verification_flag = false;
IPAddress apIP(192, 168, 4, 1);
char filePath[100];
struct QUEUE cmd_queue;
char cmd_fifo[100] = {0};
uint16_t cmd_index = 0;
WiFiUDP node_monitor;
WiFiClient serverClients[MAX_SRV_CLIENTS];
String monitor_tx_buf = "";
String monitor_rx_buf = "";
char uart_send_package[1024];
uint32_t uart_send_size;
char uart_send_package_important[1024]; //for the message that cannot missed
uint32_t uart_send_length_important;
char jsBuffer[1024];
char cloud_file_id[40];
char cloud_user_id[40];
char cloud_file_url[96];
char unbind_exec = 0;
bool upload_error = false;
bool upload_success = false;
uint32_t lastBeatTick = 0;
uint32_t lastStaticIpInfTick = 0;
unsigned long socket_busy_stamp = 0;
CLOUD_STATE cloud_state = CLOUD_NOT_CONNECT;
TRANS_STATE transfer_state = TRANSFER_IDLE;
int file_fragment = 0;
File dataFile;
int transfer_frags = 0;
char uart_rcv_package[1024];
int uart_rcv_index = 0;
boolean printFinishFlag = false;
boolean transfer_file_flag = false;
boolean rcv_end_flag = false;
uint8_t dbgStr[100] ;
OperatingState currentState = OperatingState::Unknown;

ADC_MODE(ADC_VCC);          // need this for the ESP.getVcc() call to work

uint32_t NET_INF_UPLOAD_CYCLE = 10000;

class FILE_FIFO{
  public:  
	int push(char *buf, int len){
		int i = 0;
		while(i < len ){
			if(rP != BUF_INC_POINTER(wP)){
				fifo[wP] = *(buf + i) ;
				wP = BUF_INC_POINTER(wP);
				i++;
			}else{
				break;
			}
		}
		return i;
	}
	
	int pop(char * buf, int len){	
		int i = 0;
		while(i < len){
			if(rP != wP){
				buf[i] = fifo[rP];
				rP= BUF_INC_POINTER(rP);
				i++;				
			}else{
				break;
			}
		}
		return i;
	}
	
	void reset(){		
		wP = 0;	
		rP = 0;
		memset(fifo, 0, FILE_FIFO_SIZE);
	}

	uint32_t left()	{		
		if(rP >  wP)
			return rP - wP - 1;
		else
			return FILE_FIFO_SIZE + rP - wP - 1;
	}
	
	boolean is_empty(){
		if(rP == wP)
			return true;
		else
			return false;
	}

private:
	char fifo[FILE_FIFO_SIZE];		
	uint32_t wP;	
	uint32_t rP;
};

class FILE_FIFO gFileFifo;


void init_queue(struct QUEUE *h_queue){
	
	if(h_queue == 0) return;
	
	h_queue->rd_index = 0;
	h_queue->wt_index = 0;
	memset(h_queue->buf, 0, sizeof(h_queue->buf));
}

int push_queue(struct QUEUE *h_queue, char *data_to_push, uint32_t data_len){
	if(h_queue == 0) return -1;

	if(data_len > sizeof(h_queue->buf[h_queue->wt_index])) return -1;

	if((h_queue->wt_index + 1) % QUEUE_MAX_NUM == h_queue->rd_index) return -1;

	memset(h_queue->buf[h_queue->wt_index], 0, sizeof(h_queue->buf[h_queue->wt_index]));
	memcpy(h_queue->buf[h_queue->wt_index], data_to_push, data_len);

	h_queue->wt_index = (h_queue->wt_index + 1) % QUEUE_MAX_NUM;
	
	return 0;
}

int pop_queue(struct QUEUE *h_queue, char *data_for_pop, int data_len){
	if(h_queue == 0)
		return -1;

	if((uint32_t)data_len < strlen(h_queue->buf[h_queue->rd_index]))
		return -1;

	if(h_queue->rd_index == h_queue->wt_index)
		return -1;

	memset(data_for_pop, 0, data_len);
	memcpy(data_for_pop, h_queue->buf[h_queue->rd_index], strlen(h_queue->buf[h_queue->rd_index]));

	h_queue->rd_index = (h_queue->rd_index + 1) % QUEUE_MAX_NUM;
	
	return 0;
}

bool smartConfig(){
	WiFi.mode(WIFI_STA);
	WiFi.beginSmartConfig();
	int now = millis();

	while (1){
		if(get_printer_reply() > 0){
			esp_data_parser((char *)uart_rcv_package, uart_rcv_index);
		}
		uart_rcv_index = 0;
		
		delay(1000);

		if (WiFi.smartConfigDone()){
			WiFi.stopSmartConfig();
			return true;;
		}

		if(millis() - now > 120000){ // 2min
			WiFi.stopSmartConfig();
			return false;
		}
	}
}


void net_env_prepare(){

	if(verification_flag){
		SPIFFS.begin();
		server.onNotFound(fsHandler);
	}
	
	server.servePrinter(true);
	onWifiConfig();
	server.onPrefix("/upload", HTTP_ANY, handleUpload, handleRrUpload);		
	server.begin();
	tcp.begin();
  	node_monitor.begin(UDP_PORT);
}

void reply_search_handler(){
	char packetBuffer[200];
	int packetSize = node_monitor.parsePacket();
	char  ReplyBuffer[50] = "mkswifi:";
	 
	  if (packetSize) {
	    node_monitor.read(packetBuffer, sizeof(packetBuffer));

		if(strstr(packetBuffer, "mkswifi")){
			memcpy(&ReplyBuffer[strlen("mkswifi:")], moduleId, strlen(moduleId)); 
			ReplyBuffer[strlen("mkswifi:") + strlen(moduleId)] = ',';
			if(currentState == OperatingState::Client){
				strcpy(&ReplyBuffer[strlen("mkswifi:") + strlen(moduleId) + 1], WiFi.localIP().toString().c_str()); 
				ReplyBuffer[strlen("mkswifi:") + strlen(moduleId) + strlen(WiFi.localIP().toString().c_str()) + 1] = '\n';
			}else if(currentState == OperatingState::AccessPoint){
				strcpy(&ReplyBuffer[strlen("mkswifi:") + strlen(moduleId) + 1], WiFi.softAPIP().toString().c_str()); 
				ReplyBuffer[strlen("mkswifi:") + strlen(moduleId) + strlen(WiFi.softAPIP().toString().c_str()) + 1] = '\n';
			}
			
		    node_monitor.beginPacket(node_monitor.remoteIP(), node_monitor.remotePort());
		    node_monitor.write(ReplyBuffer, strlen(ReplyBuffer));
		    node_monitor.endPacket();
		}
	  }
}

void verification(){
	verification_flag = true;
}

void var_init(){
	gPrinterInf.curBedTemp = 0.0;
	gPrinterInf.curSprayerTemp[0] = 0.0;
	gPrinterInf.curSprayerTemp[1] = 0.0;
	gPrinterInf.desireSprayerTemp[0] = 0.0;
	gPrinterInf.desireSprayerTemp[1] = 0.0;
	gPrinterInf.print_state = PRINTER_NOT_CONNECT;
	
}


void setup() {
	var_init();
	Serial.begin(115200);
	delay(20);
	EEPROM.begin(512);	
	verification();
	pinMode(McuTfrReadyPin, INPUT);
	pinMode(EspReqTransferPin, OUTPUT);
	digitalWrite(EspReqTransferPin, HIGH);

	String macStr= WiFi.macAddress();

	if (!TryToConnect()){
		StartAccessPoint();		
		currentState = OperatingState::AccessPoint;
	}

	package_net_para();
	Serial.write(uart_send_package, uart_send_size);
	net_env_prepare();   
	httpUpdater.setup(&server);
	delay(500);
}


void net_print(const uint8_t *sbuf, uint32_t len){
	for(uint32_t i = 0; i < MAX_SRV_CLIENTS; i++){
	  if (serverClients[i] && serverClients[i].connected()){
		serverClients[i].write(sbuf, len);
		delay(1);
	  }
	}
}

void query_printer_inf(){
	static int last_query_temp_time = 0;

	if((!transfer_file_flag) &&  (transfer_state == TRANSFER_IDLE)){		
		if((gPrinterInf.print_state == PRINTER_PRINTING) || (gPrinterInf.print_state == PRINTER_PAUSE)){
			if(millis() - last_query_temp_time > 5000){ //every 5 seconds
				if(GET_VERSION_OK)
					package_gcode((char *)"M27\nM992\nM994\nM991\nM997\n", false);
				else
					package_gcode((char *)"M27\nM992\nM994\nM991\nM997\nM115\n", false);
				last_query_temp_time = millis();
			}
		}else{
			if(millis() - last_query_temp_time > 5000){ //every 5 seconds
				if(GET_VERSION_OK)
					package_gcode((char *)"M991\nM27\nM997\n", false);
				else
					package_gcode((char *)"M991\nM27\nM997\nM115\n", false);
				last_query_temp_time = millis();
			}
			
		}
	}
	if((!transfer_file_flag) &&  (transfer_state == TRANSFER_IDLE)){
		if(millis() - lastBeatTick > NET_INF_UPLOAD_CYCLE){
			package_net_para();
			lastBeatTick = millis();
		}
	}
	if((manual_valid == 0xa) && (!transfer_file_flag) &&  (transfer_state == TRANSFER_IDLE)){
		if(millis() - lastStaticIpInfTick > (NET_INF_UPLOAD_CYCLE + 2)){
			package_static_ip_info();
			lastStaticIpInfTick = millis();
		}
	}
}

int get_printer_reply(void){
	size_t len = Serial.available();

	if(len > 0){
		len = ((uart_rcv_index + len) < sizeof(uart_rcv_package)) ? len : (sizeof(uart_rcv_package) - uart_rcv_index);
		Serial.readBytes(&uart_rcv_package[uart_rcv_index], len);
		uart_rcv_index += len;	

		if((uint32_t)uart_rcv_index >= sizeof(uart_rcv_package)){			
			return sizeof(uart_rcv_package);
		}
	}
	return uart_rcv_index;
}

void loop(){
	int i;
	
	switch (currentState){
		case OperatingState::Client:
			server.handleClient();
			if(verification_flag){
				cloud_handler();
			}
			break;

		case OperatingState::AccessPoint:
			server.handleClient();
			break;

		default:
			break;
	}
		
			if (tcp.hasClient()){
				for(i = 0; i < MAX_SRV_CLIENTS; i++){
				  if(serverClients[i].connected()){
					serverClients[i].stop();
				  }
				  serverClients[i] = tcp.available();
				}
				if (tcp.hasClient()){
					WiFiClient serverClient = tcp.available();
					serverClient.stop();
				}
			}
			memset(dbgStr, 0, sizeof(dbgStr));
			for(i = 0; i < MAX_SRV_CLIENTS; i++)
			{
				if (serverClients[i] && serverClients[i].connected())
				{
					uint32_t readNum = serverClients[i].available();

					if(readNum > FILE_FIFO_SIZE)
					{
					//	net_print((const uint8_t *) "readNum > FILE_FIFO_SIZE\n");
						serverClients[i].flush();
					//	Serial.println("flush"); 
						continue;
					}

				
					if(readNum > 0){
					  	uint8_t readStr[readNum + 1] ;
						uint32_t readSize;
						
						readSize = serverClients[i].read(readStr, readNum);
						readStr[readSize] = 0;
						
						if(transfer_file_flag){
							if(!verification_flag){
								break;
							}
							if(gFileFifo.left() >= readSize){
								gFileFifo.push((char *)readStr, readSize);
								transfer_frags += readSize;
							}
						}else{
							if(verification_flag){
								int j = 0;
								char cmd_line[100] = {0};
								String gcodeM3 = "";
								
								init_queue(&cmd_queue);
								
								cmd_index = 0;
								memset(cmd_fifo, 0, sizeof(cmd_fifo));
								while((uint32_t)j < readSize){
									if((readStr[j] == '\r') || (readStr[j] == '\n')){
										if((cmd_index) > 1){
											cmd_fifo[cmd_index] = '\n';
											cmd_index++;
											push_queue(&cmd_queue, cmd_fifo, cmd_index);
										}
										memset(cmd_fifo, 0, sizeof(cmd_fifo));
										cmd_index = 0;
									}else if(readStr[j] == '\0')
										break;
									else{
										if(cmd_index >= sizeof(cmd_fifo)){
											memset(cmd_fifo, 0, sizeof(cmd_fifo));
											cmd_index = 0;
										}
										cmd_fifo[cmd_index] = readStr[j];
										cmd_index++;
									}

									j++;

									do_transfer();
									yield();
								
								}
								while(pop_queue(&cmd_queue, cmd_line, sizeof(cmd_line)) >= 0){
									{
										if((strchr((const char *)cmd_line, 'G') != 0) 
											|| (strchr((const char *)cmd_line, 'M') != 0)
											|| (strchr((const char *)cmd_line, 'T') != 0))
										{
											if(strchr((const char *)cmd_line, '\n') != 0 ){
												String gcode((const char *)cmd_line);
												if(gcode.startsWith("M998") && (M3_TYPE == ROBIN)){
													net_print((const uint8_t *) "ok\r\n", strlen((const char *)"ok\r\n"));
												}else if(gcode.startsWith("M997")){
													if(gPrinterInf.print_state == PRINTER_IDLE)
														strcpy((char *)dbgStr, "M997 IDLE\r\n");
													else if(gPrinterInf.print_state == PRINTER_PRINTING)
														strcpy((char *)dbgStr, "M997 PRINTING\r\n");
													else if(gPrinterInf.print_state == PRINTER_PAUSE)
														strcpy((char *)dbgStr, "M997 PAUSE\r\n");
													else
														strcpy((char *)dbgStr, "M997 NOT CONNECTED\r\n");
												}else if(gcode.startsWith("M27")){
													memset(dbgStr, 0, sizeof(dbgStr));
													sprintf((char *)dbgStr, "M27 %d\r\n", gPrinterInf.print_file_inf.print_rate);
												}else if(gcode.startsWith("M992")){
													memset(dbgStr, 0, sizeof(dbgStr));
													sprintf((char *)dbgStr, "M992 %02d:%02d:%02d\r\n", 
													gPrinterInf.print_file_inf.print_hours, gPrinterInf.print_file_inf.print_mins, gPrinterInf.print_file_inf.print_seconds);
												}else if(gcode.startsWith("M994")){
													memset(dbgStr, 0, sizeof(dbgStr));
													sprintf((char *)dbgStr, "M994 %s;%d\r\n", 
													gPrinterInf.print_file_inf.file_name.c_str(), gPrinterInf.print_file_inf.file_size);														
												}else  if(gcode.startsWith("M115")){
													memset(dbgStr, 0, sizeof(dbgStr));
													if(M3_TYPE == ROBIN)
														strcpy((char *)dbgStr, "FIRMWARE_NAME:Robin\r\n");
													else if(M3_TYPE == TFT28)
														strcpy((char *)dbgStr, "FIRMWARE_NAME:TFT28/32\r\n");
													else if(M3_TYPE == TFT24)
														strcpy((char *)dbgStr, "FIRMWARE_NAME:TFT24\r\n");
												}else{	
													if(gPrinterInf.print_state == PRINTER_IDLE){
														if(gcode.startsWith("M23") || gcode.startsWith("M24")){
														 	gPrinterInf.print_state = PRINTER_PRINTING;
															gPrinterInf.print_file_inf.file_name = "";
															gPrinterInf.print_file_inf.file_size = 0;
															gPrinterInf.print_file_inf.print_rate = 0;
															gPrinterInf.print_file_inf.print_hours = 0;
															gPrinterInf.print_file_inf.print_mins = 0;
															gPrinterInf.print_file_inf.print_seconds = 0;

															printFinishFlag = false;
														 }
													}
													gcodeM3.concat(gcode);
												}
											}
										}
									}
									if(strlen((const char *)dbgStr) > 0){
										net_print((const uint8_t *) "ok\r\n", strlen((const char *)"ok\r\n"));
										net_print((const uint8_t *) dbgStr, strlen((const char *)dbgStr));		
										memset(dbgStr, 0, sizeof(dbgStr));			
									}
								
									do_transfer();
									yield();
									
								}
								
								if(gcodeM3.length() > 2){
									package_gcode(gcodeM3, true);
									do_transfer();
									socket_busy_stamp = millis();
								}
							}
						}
					}
				}
			}
		
			do_transfer();
			if(get_printer_reply() > 0){
				esp_data_parser((char *)uart_rcv_package, uart_rcv_index);
			}
			uart_rcv_index = 0;

		if(verification_flag){
			query_printer_inf();	
			if(millis() - socket_busy_stamp > 5000){				
				reply_search_handler();
			}
			cloud_down_file();
			cloud_get_file_list();
		}else{
			verification();
		}
	
	yield();
}


int package_net_para(void){
	int dataLen;
	int wifi_name_len=0;
	int wifi_key_len=0;
	
	memset(uart_send_package, 0, sizeof(uart_send_package));
	uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
	uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_NET;

	if(currentState == OperatingState::Client)
	{
		
		if(WiFi.status() == WL_CONNECTED)
		{
			uart_send_package[UART_PROTCL_DATA_OFFSET] = WiFi.localIP()[0];
			uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = WiFi.localIP()[1];
			uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = WiFi.localIP()[2];
			uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = WiFi.localIP()[3];
			uart_send_package[UART_PROTCL_DATA_OFFSET + 6] = 0x0a;
		}
		else
		{
			uart_send_package[UART_PROTCL_DATA_OFFSET] = 0;
			uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = 0;
			uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = 0;
			uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = 0;
			uart_send_package[UART_PROTCL_DATA_OFFSET + 6] = 0x05;
		}

		uart_send_package[UART_PROTCL_DATA_OFFSET + 7] = 0x02;

		wifi_name_len = strlen(ssid);
		wifi_key_len = strlen(pass);

		uart_send_package[UART_PROTCL_DATA_OFFSET + 8] = wifi_name_len;

		strcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 9], ssid);

		uart_send_package[UART_PROTCL_DATA_OFFSET + 9 + wifi_name_len] = wifi_key_len;

		strcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 10 + wifi_name_len], pass);	

		
	} 
	else if(currentState == OperatingState::AccessPoint)
	{
		uart_send_package[UART_PROTCL_DATA_OFFSET] = WiFi.softAPIP()[0];
		uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = WiFi.softAPIP()[1];
		uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = WiFi.softAPIP()[2];
		uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = WiFi.softAPIP()[3];
		
		uart_send_package[UART_PROTCL_DATA_OFFSET + 6] = 0x0a;
		uart_send_package[UART_PROTCL_DATA_OFFSET + 7] = 0x01;

		
		wifi_name_len = strlen(softApName);
		wifi_key_len = strlen(softApKey);

		uart_send_package[UART_PROTCL_DATA_OFFSET + 8] = wifi_name_len;

		strcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 9], softApName);

		uart_send_package[UART_PROTCL_DATA_OFFSET + 9 + wifi_name_len] = wifi_key_len;

		strcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 10 + wifi_name_len], softApKey);	
		
		
	}

	int host_len = strlen((const char *)cloud_host);
	
	if(cloud_enable_flag)
	{
		if(cloud_link_state == 3)
			uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 10] =0x12;
		else if( (cloud_link_state == 1) || (cloud_link_state == 2))
			uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 10] =0x11;
		else if(cloud_link_state == 0)
			uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 10] =0x10;
	}
	else
	{
		uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 10] =0x0;
	
	}

	uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 11] = host_len;
	memcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + 12], cloud_host, host_len);
	uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + 12] = cloud_port & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + 13] = (cloud_port >> 8 ) & 0xff;

	int id_len = strlen((const char *)moduleId);
	uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + 14]  = id_len;
	memcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + 15], moduleId, id_len);
		
	int ver_len = strlen((const char *)firmwareVersion);
	uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + id_len + 15]  = ver_len;
	memcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + wifi_name_len + wifi_key_len + host_len + id_len + 16], firmwareVersion, ver_len);
		
	dataLen = wifi_name_len + wifi_key_len + host_len + id_len + ver_len + 16;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 4] = 8080 & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 5] = (8080 >> 8 )& 0xff;

	if(!verification_flag)
		uart_send_package[UART_PROTCL_DATA_OFFSET + 6] = 0x0e;

	
	uart_send_package[UART_PROTCL_DATALEN_OFFSET] = dataLen & 0xff;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = dataLen >> 8;
	
	
	uart_send_package[dataLen + 4] = UART_PROTCL_TAIL;

	uart_send_size = dataLen + 5;

	return 0;
}

int package_static_ip_info(void){
/*
	int dataLen;
	int wifi_name_len;
	int wifi_key_len;
*/
	
	memset(uart_send_package, 0, sizeof(uart_send_package));
	uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
	uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_STATIC_IP;

	uart_send_package[UART_PROTCL_DATA_OFFSET] = ip_static & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = (ip_static >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = (ip_static >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = (ip_static >> 24) & 0xff;

	uart_send_package[UART_PROTCL_DATA_OFFSET + 4] = subnet_static & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 5] = (subnet_static >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 6] = (subnet_static >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 7] = (subnet_static >> 24) & 0xff;

	uart_send_package[UART_PROTCL_DATA_OFFSET + 8] = gateway_staic & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 9] = (gateway_staic >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 10] = (gateway_staic >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 11] = (gateway_staic >> 24) & 0xff;

	uart_send_package[UART_PROTCL_DATA_OFFSET + 12] = dns_static & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 13] = (dns_static >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 14] = (dns_static >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 15] = (dns_static >> 24) & 0xff;

	uart_send_package[UART_PROTCL_DATALEN_OFFSET] = 16;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = 0;	
	
	uart_send_package[UART_PROTCL_DATA_OFFSET + 16] = UART_PROTCL_TAIL;

	uart_send_size = UART_PROTCL_DATA_OFFSET + 17;
	return 0;
}

int package_gcode(String gcodeStr, boolean important)
{
	int dataLen;
	const char *dataField = gcodeStr.c_str();
	
	uint32_t buffer_offset;
	
	dataLen = strlen(dataField);
	
	if(dataLen > 1019)
		return -1;

	if(important)
	{	
		buffer_offset = uart_send_length_important;
	}
	else
	{		
		buffer_offset = 0;
		memset(uart_send_package, 0, sizeof(uart_send_package));
	}
	
	if(dataLen + buffer_offset > 1019)
		return -1;
	
	if(important){
		uart_send_package_important[UART_PROTCL_HEAD_OFFSET + buffer_offset] = UART_PROTCL_HEAD;
		uart_send_package_important[UART_PROTCL_TYPE_OFFSET + buffer_offset] = UART_PROTCL_TYPE_GCODE;
		uart_send_package_important[UART_PROTCL_DATALEN_OFFSET + buffer_offset] = dataLen & 0xff;
		uart_send_package_important[UART_PROTCL_DATALEN_OFFSET + buffer_offset + 1] = dataLen >> 8;
		
		strncpy(&uart_send_package_important[UART_PROTCL_DATA_OFFSET + buffer_offset], dataField, dataLen);
		
		uart_send_package_important[dataLen + buffer_offset + 4] = UART_PROTCL_TAIL;

		uart_send_length_important += dataLen + 5;
	}else{	
		uart_send_package[UART_PROTCL_HEAD_OFFSET + buffer_offset] = UART_PROTCL_HEAD;
		uart_send_package[UART_PROTCL_TYPE_OFFSET + buffer_offset] = UART_PROTCL_TYPE_GCODE;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET + buffer_offset] = dataLen & 0xff;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET + buffer_offset + 1] = dataLen >> 8;
		strncpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + buffer_offset], dataField, dataLen);
		uart_send_package[dataLen + buffer_offset + 4] = UART_PROTCL_TAIL;
		uart_send_size = dataLen + 5;
	}

	if(monitor_tx_buf.length() + gcodeStr.length() < 300){
		monitor_tx_buf.concat(gcodeStr);
	}
	return 0;
}

int package_gcode(char *dataField, boolean important){	
	uint32_t buffer_offset;
	int dataLen = strlen((const char *)dataField);

	if(important)
	{		
		
		buffer_offset = uart_send_length_important;
	}
	else
	{
		buffer_offset = 0;
		memset(uart_send_package, 0, sizeof(uart_send_package));
	}
	if(dataLen + buffer_offset > 1019)
		return -1;
	//net_print((const uint8_t *)"dataField:", strlen("dataField:"));
	//net_print((const uint8_t *)dataField, strlen(dataField));
	//net_print((const uint8_t *)"\n", 1);

	/**(buffer_to_send + UART_PROTCL_HEAD_OFFSET + buffer_offset) = UART_PROTCL_HEAD;
	*(buffer_to_send + UART_PROTCL_TYPE_OFFSET + buffer_offset) = UART_PROTCL_TYPE_GCODE;
	*(buffer_to_send + UART_PROTCL_DATALEN_OFFSET + buffer_offset) = dataLen & 0xff;
	*(buffer_to_send + UART_PROTCL_DATALEN_OFFSET + buffer_offset + 1) = dataLen >> 8;
	strncpy(buffer_to_send + UART_PROTCL_DATA_OFFSET + buffer_offset, dataField, dataLen);
	
	*(buffer_to_send + dataLen + buffer_offset + 4) = UART_PROTCL_TAIL;
*/
	
	if(important)
	{
		uart_send_package_important[UART_PROTCL_HEAD_OFFSET + buffer_offset] = UART_PROTCL_HEAD;
		uart_send_package_important[UART_PROTCL_TYPE_OFFSET + buffer_offset] = UART_PROTCL_TYPE_GCODE;
		uart_send_package_important[UART_PROTCL_DATALEN_OFFSET + buffer_offset] = dataLen & 0xff;
		uart_send_package_important[UART_PROTCL_DATALEN_OFFSET + buffer_offset + 1] = dataLen >> 8;
		
		strncpy(&uart_send_package_important[UART_PROTCL_DATA_OFFSET + buffer_offset], dataField, dataLen);
		
		uart_send_package_important[dataLen + buffer_offset + 4] = UART_PROTCL_TAIL;

		uart_send_length_important += dataLen + 5;
	}
	else
	{	
		uart_send_package[UART_PROTCL_HEAD_OFFSET + buffer_offset] = UART_PROTCL_HEAD;
		uart_send_package[UART_PROTCL_TYPE_OFFSET + buffer_offset] = UART_PROTCL_TYPE_GCODE;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET + buffer_offset] = dataLen & 0xff;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET + buffer_offset + 1] = dataLen >> 8;
		
		strncpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + buffer_offset], dataField, dataLen);
		
		uart_send_package[dataLen + buffer_offset + 4] = UART_PROTCL_TAIL;

		uart_send_size = dataLen + 5;
	}
	
	
	
	if(monitor_tx_buf.length() + strlen(dataField) < 300)
	{
		monitor_tx_buf.concat(dataField);
	}
	else
	{
		//net_print((const uint8_t *)"overflow", strlen("overflow"));
	}
	return 0;
}



int package_file_first(File *fileHandle, char *fileName)
{
	int fileLen;
	char *ptr;
	int fileNameLen;
	int dataLen;
	
	if(fileHandle == 0)
		return -1;

	fileLen = fileHandle->size();
	
	while(1){
		ptr = (char *)strchr(fileName, '/');
		if(ptr == 0)
			break;
		else{
			strcpy(fileName, fileName + (ptr - fileName+ 1));
		}
	}
	fileNameLen = strlen(fileName);

	dataLen = fileNameLen + 5;

	memset(uart_send_package, 0, sizeof(uart_send_package));
	uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
	uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_FIRST;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET] = dataLen & 0xff;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = dataLen >> 8;

	uart_send_package[UART_PROTCL_DATA_OFFSET] = fileNameLen;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = fileLen & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = (fileLen >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = (fileLen >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 4] = (fileLen >> 24) & 0xff;
	strncpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 5], fileName, fileNameLen);
	
	uart_send_package[dataLen + 4] = UART_PROTCL_TAIL;
	
	uart_send_size = dataLen + 5;

	return 0;
}

int package_file_first(char *fileName, int postLength)
{
	int fileLen;
	char *ptr;
	int fileNameLen;
	int dataLen;

	
	fileLen = postLength;
	
//	Serial.print("package_file_first:");
	
	while(1)
	{
		ptr = (char *)strchr(fileName, '/');
		if(ptr == 0)
			break;
		else
		{
			cut_msg_head((uint8_t *)fileName, strlen(fileName),  ptr - fileName+ 1);
		}
	}
//	Serial.print("fileName:");
//	Serial.println(fileName);
	
	fileNameLen = strlen(fileName);

	dataLen = fileNameLen + 5;

	memset(uart_send_package, 0, sizeof(uart_send_package));
	uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
	uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_FIRST;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET] = dataLen & 0xff;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = dataLen >> 8;

	uart_send_package[UART_PROTCL_DATA_OFFSET] = fileNameLen;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = fileLen & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = (fileLen >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = (fileLen >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 4] = (fileLen >> 24) & 0xff;
	memcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 5], fileName, fileNameLen);
	
	uart_send_package[dataLen + 4] = UART_PROTCL_TAIL;
	
	uart_send_size = dataLen + 5;

	return 0;
}


int package_file_fragment(uint8_t *dataField, uint32_t fragLen, int32_t fragment){
	int dataLen;
	dataLen = fragLen + 4;
	
	memset(uart_send_package, 0, sizeof(uart_send_package));
	uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
	uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_FRAGMENT;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET] = dataLen & 0xff;
	uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = dataLen >> 8;

	uart_send_package[UART_PROTCL_DATA_OFFSET] = fragment & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 1] = (fragment >> 8) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 2] = (fragment >> 16) & 0xff;
	uart_send_package[UART_PROTCL_DATA_OFFSET + 3] = (fragment >> 24) & 0xff;
	
	memcpy(&uart_send_package[UART_PROTCL_DATA_OFFSET + 4], (const char *)dataField, fragLen);
	
	uart_send_package[dataLen + 4] = UART_PROTCL_TAIL;
	uart_send_size = 1024;

	return 0;
}

unsigned long startTick = 0;
size_t readBytes;
uint8_t blockBuf[FILE_BLOCK_SIZE] = {0};
	

void do_transfer(void){
/*
	char dbgStr[100] = {0};
	int i;
	long useTick ;
	long now;
*/	
	switch(transfer_state){
		case TRANSFER_IDLE:
			if((uart_send_length_important > 0) || (uart_send_size > 0)){
				digitalWrite(EspReqTransferPin, LOW);
				if(digitalRead(McuTfrReadyPin) == LOW){ // STM32 READY SIGNAL
					transfer_state = TRANSFER_FRAGMENT;
				}else{
					transfer_state = TRANSFER_READY;
				}
			}
			break;
			
		case TRANSFER_GET_FILE:
			if(Serial.baudRate() != 1958400){
				Serial.flush();
				Serial.begin(1958400);
			}
			
			readBytes = gFileFifo.pop((char *)blockBuf, FILE_BLOCK_SIZE);
			if(readBytes > 0){
				if(rcv_end_flag && (readBytes < FILE_BLOCK_SIZE)){
					file_fragment |= (1 << 31);	
				}else{
					file_fragment &= ~(1 << 31);
				}

				package_file_fragment(blockBuf, readBytes, file_fragment);
				digitalWrite(EspReqTransferPin, LOW);
				transfer_state = TRANSFER_READY;
				file_fragment++;
			}else if(rcv_end_flag){
				memset(blockBuf, 0, sizeof(blockBuf));
				readBytes = 0;
				file_fragment |= (1 << 31);	//the last fragment
				package_file_fragment(blockBuf, readBytes, file_fragment);
				digitalWrite(EspReqTransferPin, LOW);
				transfer_state = TRANSFER_READY;
			}
			break;

		case TRANSFER_READY:
			if(digitalRead(McuTfrReadyPin) == LOW){ // STM32 READY SIGNAL
			{
				transfer_state = TRANSFER_FRAGMENT;
			}
				
			break;
			
		case TRANSFER_FRAGMENT:
			if(uart_send_length_important > 0){
				uart_send_length_important = (uart_send_length_important >= sizeof(uart_send_package_important) ? sizeof(uart_send_package_important) : uart_send_length_important);
				Serial.write(uart_send_package_important, uart_send_length_important);
				uart_send_length_important = 0;
				memset(uart_send_package_important, 0, sizeof(uart_send_package_important));
			}else{
				Serial.write(uart_send_package, uart_send_size);
				uart_send_size = 0;
				memset(uart_send_package, 0, sizeof(uart_send_package));
			}
			digitalWrite(EspReqTransferPin, HIGH);

			if(!transfer_file_flag){
				transfer_state = TRANSFER_IDLE;
			}else{
				if(rcv_end_flag && (readBytes < FILE_BLOCK_SIZE)){							
					
					if(Serial.baudRate() != 115200){
						Serial.flush();
						Serial.begin(115200);
					}
					transfer_file_flag = false;
					rcv_end_flag = false;
					transfer_state = TRANSFER_IDLE;
				}else{
					transfer_state = TRANSFER_GET_FILE;
				}
			}
			break;
			
		default:
			break;
	}
	if(transfer_file_flag){
		if((gFileFifo.left() >= TCP_FRAG_LEN) && (transfer_frags >= TCP_FRAG_LEN)){
			net_print((const uint8_t *) "ok\n", strlen((const char *)"ok\n"));
			transfer_frags -= TCP_FRAG_LEN;
		}
	}
}
};



uint8_t esp_msg_buf[UART_RX_BUFFER_SIZE] = {0};
uint16_t esp_msg_index = 0;


int32_t charAtArray(const uint8_t *_array, uint32_t _arrayLen, uint8_t _char){
	uint32_t i;
	for(i = 0; i < _arrayLen; i++)
	{
		if(*(_array + i) == _char)
		{
			return i;
		}
	}
	
	return -1;
}

int cut_msg_head(uint8_t *msg, uint16_t msgLen, uint16_t cutLen){
	
	if(msgLen < cutLen){
		return 0;
	}else if(msgLen == cutLen){
		memset(msg, 0, msgLen);
		return 0;
	}
	
	for(int i = 0; i < (msgLen - cutLen); i++){
		msg[i] = msg[cutLen + i];
	}
	
	memset(&msg[msgLen - cutLen], 0, cutLen);
	
	return msgLen - cutLen;
}




void net_msg_handle(uint8_t *msg, uint16_t msgLen){
	uint8_t cfg_mode;
	uint8_t cfg_wifi_len;
	uint8_t *cfg_wifi;
	uint8_t cfg_key_len;
	uint8_t *cfg_key;

	char valid[1] = {0x0a};

	if(msgLen <= 0)
		return;

	if((msg[0] != 0x01) && (msg[0] != 0x02)) 
		return;

	cfg_mode = msg[0];

	if(msg[1] > 32)
		return;

	cfg_wifi_len = msg[1];
	cfg_wifi = &msg[2];
	
	if(msg[2 +cfg_wifi_len ] > 64)
		return;

	cfg_key_len = msg[2 +cfg_wifi_len];
	cfg_key = &msg[3 +cfg_wifi_len];

	
	
	if((cfg_mode == 0x01) && ((currentState == OperatingState::Client) 
		|| (cfg_wifi_len != strlen((const char *)softApName))
		|| (strncmp((const char *)cfg_wifi, (const char *)softApName, cfg_wifi_len) != 0)
		|| (cfg_key_len != strlen((const char *)softApKey))
		|| (strncmp((const char *)cfg_key,  (const char *)softApKey, cfg_key_len) != 0)))
	{
		if((cfg_key_len > 0) && (cfg_key_len < 8))
		{
			return;
		}
		
		memset(softApName, 0, sizeof(softApName));
		memset(softApKey, 0, sizeof(softApKey));
		memset(wifi_mode, 0, sizeof(wifi_mode));
		
		strncpy((char *)softApName, (const char *)cfg_wifi, cfg_wifi_len);
		strncpy((char *)softApKey, (const char *)cfg_key, cfg_key_len);

	
		strcpy((char *)wifi_mode, "wifi_mode_ap");
		
		
		EEPROM.put(BAK_ADDRESS_WIFI_SSID, softApName);
		EEPROM.put(BAK_ADDRESS_WIFI_KEY, softApKey);
		
		EEPROM.put(BAK_ADDRESS_WIFI_MODE, wifi_mode);

		EEPROM.put(BAK_ADDRESS_WIFI_VALID, valid);
		
		EEPROM.commit();
		delay(300);
		ESP.restart();
	}
	else if((cfg_mode == 0x02) && ((currentState == OperatingState::AccessPoint)
		|| (cfg_wifi_len != strlen((const char *)ssid))
		|| (strncmp((const char *)cfg_wifi, (const char *)ssid, cfg_wifi_len) != 0)
		|| (cfg_key_len != strlen((const char *)pass))
		|| (strncmp((const char *)cfg_key,  (const char *)pass, cfg_key_len) != 0)))
	{
		memset(ssid, 0, sizeof(ssid));
		memset(pass, 0, sizeof(pass));
		memset(wifi_mode, 0, sizeof(wifi_mode));
		strncpy((char *)ssid, (const char *)cfg_wifi, cfg_wifi_len);
		strncpy((char *)pass, (const char *)cfg_key, cfg_key_len);
		
		strcpy((char *)wifi_mode, "wifi_mode_sta");	
		
		EEPROM.put(BAK_ADDRESS_WIFI_SSID, ssid);
		EEPROM.put(BAK_ADDRESS_WIFI_KEY, pass);

		EEPROM.put(BAK_ADDRESS_WIFI_MODE, wifi_mode);

		EEPROM.put(BAK_ADDRESS_WIFI_VALID, valid);

		//Disable manual ip mode		
		EEPROM.put(BAK_ADDRESS_MANUAL_IP_FLAG, 0xff);
		manual_valid = 0xff;

		EEPROM.commit();
		
		delay(300);
		ESP.restart();
	}
	
}

void cloud_msg_handle(uint8_t * msg, uint16_t msgLen){
//Todo
}


void scan_wifi_msg_handle(void){
	uint8_t valid_nums = 0;
	uint32_t byte_offset = 1;
	uint8_t node_lenth;
	int8_t signal_rssi;
	
	if(currentState == OperatingState::AccessPoint)
	{
		WiFi.mode(WIFI_STA);
		WiFi.disconnect();
		delay(100);
	}
	
	int n = WiFi.scanNetworks();
//	Serial.println("scan done");
	if (n == 0)
	{
		//Serial.println("no networks found");
		return;
	}else{
		memset(uart_send_package, 0, sizeof(uart_send_package));
		uart_send_package[UART_PROTCL_HEAD_OFFSET] = UART_PROTCL_HEAD;
		uart_send_package[UART_PROTCL_TYPE_OFFSET] = UART_PROTCL_TYPE_HOT_PORT;
		for (int i = 0; i < n; ++i){
			if(valid_nums > 15)
				break;
			signal_rssi = (int8_t)WiFi.RSSI(i);
			node_lenth = (uint8_t)WiFi.SSID(i).length();

			if(node_lenth > 32){					
				continue;
			}	
			
			if(signal_rssi < -78){
				continue;
			}

			uart_send_package[UART_PROTCL_DATA_OFFSET + byte_offset] = node_lenth;
			WiFi.SSID(i).toCharArray(&uart_send_package[UART_PROTCL_DATA_OFFSET + byte_offset + 1], node_lenth + 1, 0);
			uart_send_package[UART_PROTCL_DATA_OFFSET + byte_offset + node_lenth + 1] = WiFi.RSSI(i);

			valid_nums++;
			byte_offset += node_lenth + 2;
			
		}
		
		uart_send_package[UART_PROTCL_DATA_OFFSET] = valid_nums;
		uart_send_package[UART_PROTCL_DATA_OFFSET + byte_offset] = UART_PROTCL_TAIL;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET] = byte_offset & 0xff;
		uart_send_package[UART_PROTCL_DATALEN_OFFSET + 1] = byte_offset >> 8;

		uart_send_size = byte_offset + 5;

		/*if(transfer_state == TRANSFER_IDLE)
		{
			transfer_state = TRANSFER_READY;
			digitalWrite(EspReqTransferPin, LOW);
		}*/
		
	}
	//Serial.println("");
}


static void manual_ip_msg_handle(uint8_t * msg, uint16_t msgLen)
{
	
	if(msgLen < 16)
		return;
		
	ip_static = (msg[3] << 24) + (msg[2] << 16) + (msg[1] << 8) + msg[0];
	subnet_static = (msg[7] << 24) + (msg[6] << 16) + (msg[5] << 8) + msg[4];
	gateway_staic = (msg[11] << 24) + (msg[10] << 16) + (msg[9] << 8) + msg[8];
	dns_static = (msg[15] << 24) + (msg[14] << 16) + (msg[13] << 8) + msg[12];

	manual_valid = 0xa;
	
	WiFi.config(ip_static, gateway_staic, subnet_static, dns_static, (uint32_t)0x00000000);

	EEPROM.put(BAK_ADDRESS_MANUAL_IP, ip_static);
	EEPROM.put(BAK_ADDRESS_MANUAL_MASK, subnet_static);
	EEPROM.put(BAK_ADDRESS_MANUAL_GATEWAY, gateway_staic);
	EEPROM.put(BAK_ADDRESS_MANUAL_DNS, dns_static);
	EEPROM.put(BAK_ADDRESS_MANUAL_IP_FLAG, manual_valid);

	EEPROM.commit();

	
	
}

static void wifi_ctrl_msg_handle(uint8_t * msg, uint16_t msgLen)
{
	if(msgLen != 1)
		return;

	uint8_t ctrl_code = msg[0];

	/*connect the wifi network*/
	if(ctrl_code == 0x1)
	{
		if(!WiFi.isConnected())
		{
			WiFi.begin(ssid, pass);
		}
	}
	/*disconnect the wifi network*/
	else if(ctrl_code == 0x2)
	{
		if(WiFi.isConnected())
		{
			WiFi.disconnect();
		}
	}
	/*disconnect the wifi network and forget the password*/
	else if(ctrl_code == 0x3)
	{
		if(WiFi.isConnected())
		{
			WiFi.disconnect();
		}
		memset(ssid, 0, sizeof(ssid));
		memset(pass, 0, sizeof(pass));
	
				
		EEPROM.put(BAK_ADDRESS_WIFI_SSID, ssid);
		EEPROM.put(BAK_ADDRESS_WIFI_KEY, pass);

		EEPROM.put(BAK_ADDRESS_WIFI_VALID, 0Xff);

		//Disable manual ip mode		
		EEPROM.put(BAK_ADDRESS_MANUAL_IP_FLAG, 0xff);
		manual_valid = 0xff;

		EEPROM.commit();
	}
}

static void except_msg_handle(uint8_t * msg, uint16_t msgLen)
{
	uint8_t except_code = msg[0];
	if(except_code == 0x1) // transfer error
	{
		upload_error = true;
	}
	else if(except_code == 0x2) // transfer sucessfully
	{
		upload_success = true;
	}
}

static void wid_msg_handle(uint8_t * msg, uint16_t msgLen)
{
//Todo
}

static void transfer_msg_handle(uint8_t * msg, uint16_t msgLen)
{
	int j = 0;
	char cmd_line[100] = {0};
	
	init_queue(&cmd_queue);
	cmd_index = 0;
	memset(cmd_fifo, 0, sizeof(cmd_fifo));
	
	while(j < msgLen)
	{
		if((msg[j] == '\r') || (msg[j] == '\n'))
		{
			if((cmd_index) > 1)
			{
				cmd_fifo[cmd_index] = '\n';
				cmd_index++;

				
				push_queue(&cmd_queue, cmd_fifo, cmd_index);
			}
			memset(cmd_fifo, 0, sizeof(cmd_fifo));
			cmd_index = 0;
		}
		else if(msg[j] == '\0')
			break;
		else
		{
			if(cmd_index >= sizeof(cmd_fifo))
			{
				memset(cmd_fifo, 0, sizeof(cmd_fifo));
				cmd_index = 0;
			}
			cmd_fifo[cmd_index] = msg[j];
			cmd_index++;
		}

		j++;

		do_transfer();
		yield();
	
	}
	while(pop_queue(&cmd_queue, cmd_line, sizeof(cmd_line)) >= 0)		
	{
		if(monitor_rx_buf.length() + strlen(cmd_line) < 500)
		{
			monitor_rx_buf.concat((const char *)cmd_line);
		}
		else
		{
			//net_print((const uint8_t *)"rx overflow", strlen("rx overflow"));
		}
		/*
		if((cmd_line[0] == 'o') && (cmd_line[1] == 'k'))
		{
			cut_msg_head((uint8_t *)cmd_line, strlen((const char*)cmd_line), 2);
			//if(strlen(cmd_line) < 4)
				continue;
		}*/

		/*handle the cmd*/
		paser_cmd((uint8_t *)cmd_line);
		do_transfer();
		yield();
		
		if((cmd_line[0] == 'T') && (cmd_line[1] == ':'))
		{		
			String tempVal((const char *)cmd_line);
			int index = tempVal.indexOf("B:", 0);
			if(index != -1)			
			{
				memset(dbgStr, 0, sizeof(dbgStr));
				sprintf((char *)dbgStr, "T:%d /%d B:%d /%d T0:%d /%d T1:%d /%d @:0 B@:0\r\n", 
					(int)gPrinterInf.curSprayerTemp[0], (int)gPrinterInf.desireSprayerTemp[0], (int)gPrinterInf.curBedTemp, (int)gPrinterInf.desireBedTemp,
					(int)gPrinterInf.curSprayerTemp[0], (int)gPrinterInf.desireSprayerTemp[0], (int)gPrinterInf.curSprayerTemp[1], (int)gPrinterInf.desireSprayerTemp[1]);
				net_print((const uint8_t*)dbgStr, strlen((const char *)dbgStr));
					
			}
			continue;
		}
		else if((cmd_line[0] == 'M') && (cmd_line[1] == '9') && (cmd_line[2] == '9') 
			&& ((cmd_line[3] == '7') ||  (cmd_line[3] == '2') ||  (cmd_line[3] == '4')))
		{
			continue;
		}
		else if((cmd_line[0] == 'M') && (cmd_line[1] == '2') && (cmd_line[2] == '7'))
		{
			continue;
		}
		else
		{
			net_print((const uint8_t*)cmd_line, strlen((const char *)cmd_line));
		}
		
		
	}
	
	
	
}

void esp_data_parser(char *cmdRxBuf, int len)
{
	int32_t head_pos;
	int32_t tail_pos;
	uint16_t cpyLen;
	uint16_t leftLen = len; 
	uint8_t loop_again = 0;
	ESP_PROTOC_FRAME esp_frame;
	
	
	while((leftLen > 0) || (loop_again == 1)){
		loop_again = 0;
		
		if(esp_msg_index != 0){
			head_pos = 0;
			cpyLen = (leftLen < (sizeof(esp_msg_buf) - esp_msg_index)) ? leftLen : sizeof(esp_msg_buf) - esp_msg_index;
			
			memcpy(&esp_msg_buf[esp_msg_index], cmdRxBuf + len - leftLen, cpyLen);			

			esp_msg_index += cpyLen;

			leftLen = leftLen - cpyLen;
			tail_pos = charAtArray(esp_msg_buf, esp_msg_index, ESP_PROTOC_TAIL);
			
			if(tail_pos == -1){
				if(esp_msg_index >= sizeof(esp_msg_buf)){
					memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
					esp_msg_index = 0;
				}
			
				return;
			}
		}else{
			head_pos = charAtArray((uint8_t const *)&cmdRxBuf[len - leftLen], leftLen, ESP_PROTOC_HEAD);
			if(head_pos == -1){
				return;
			}else{
				memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
				memcpy(esp_msg_buf, &cmdRxBuf[len - leftLen + head_pos], leftLen - head_pos);

				esp_msg_index = leftLen - head_pos;
				leftLen = 0;
				head_pos = 0;
				tail_pos = charAtArray(esp_msg_buf, esp_msg_index, ESP_PROTOC_TAIL);
				if(tail_pos == -1){
					return;
				}
			}
		}

		esp_frame.type = esp_msg_buf[1];
	
		if((esp_frame.type != ESP_TYPE_NET) && (esp_frame.type != ESP_TYPE_PRINTER)
			 && (esp_frame.type != ESP_TYPE_CLOUD) && (esp_frame.type != ESP_TYPE_UNBIND)
			 && (esp_frame.type != ESP_TYPE_TRANSFER) && (esp_frame.type != ESP_TYPE_EXCEPTION) 
			 && (esp_frame.type != ESP_TYPE_WID) && (esp_frame.type != ESP_TYPE_SCAN_WIFI)
			 && (esp_frame.type != ESP_TYPE_MANUAL_IP) && (esp_frame.type != ESP_TYPE_WIFI_CTRL))
		{
			memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
			esp_msg_index = 0;
			return;
		}
		esp_frame.dataLen = esp_msg_buf[2] + (esp_msg_buf[3] << 8);

		if((uint16_t)(esp_frame.dataLen+4) > sizeof(esp_msg_buf)){
			memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
			esp_msg_index = 0;
			return;
		}

		if(esp_msg_buf[4 + esp_frame.dataLen] != ESP_PROTOC_TAIL){
			memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
			esp_msg_index = 0;
			return;
		}
	
		esp_frame.data = &esp_msg_buf[4];
	
		switch(esp_frame.type)
		{
			case ESP_TYPE_NET:
				net_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_PRINTER:
				//gcode_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_TRANSFER:
				if(verification_flag)
					transfer_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_CLOUD:
				if(verification_flag)
					cloud_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_EXCEPTION:
				
				except_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_UNBIND:
				if(cloud_link_state == 3){
					unbind_exec = 1;
				}
				break;
			case ESP_TYPE_WID:
				wid_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_SCAN_WIFI:
				scan_wifi_msg_handle();
				break;

			case ESP_TYPE_MANUAL_IP:				
				manual_ip_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;

			case ESP_TYPE_WIFI_CTRL:				
				wifi_ctrl_msg_handle(esp_frame.data, esp_frame.dataLen);
				break;
			
			default:
				break;				
		}

		esp_msg_index = cut_msg_head(esp_msg_buf, esp_msg_index, esp_frame.dataLen  + 5);
		if(esp_msg_index > 0){
			if(charAtArray(esp_msg_buf, esp_msg_index,  ESP_PROTOC_HEAD) == -1){
				memset(esp_msg_buf, 0, sizeof(esp_msg_buf));
				esp_msg_index = 0;
				return;
			}
			
			if((charAtArray(esp_msg_buf, esp_msg_index,  ESP_PROTOC_HEAD) != -1) && (charAtArray(esp_msg_buf, esp_msg_index, ESP_PROTOC_TAIL) != -1)){
				loop_again = 1;
			}
		}
		yield();
	
	}
}



// Try to connect using the saved SSID and password, returning true if successful
bool TryToConnect(){
	char eeprom_valid[1] = {0};
	uint8_t failcount = 0;
	
	EEPROM.get(BAK_ADDRESS_WIFI_VALID, eeprom_valid);
	if(eeprom_valid[0] == 0x0a){
		EEPROM.get(BAK_ADDRESS_WIFI_MODE, wifi_mode);		
		EEPROM.get(BAK_ADDRESS_WEB_HOST, webhostname);
	}else{
		memset(wifi_mode, 0, sizeof(wifi_mode));
		strcpy(wifi_mode, "wifi_mode_ap");
		NET_INF_UPLOAD_CYCLE = 1000;
	}

	if(strcmp(wifi_mode, "wifi_mode_ap") != 0){	
		if(eeprom_valid[0] == 0x0a){
			EEPROM.get(BAK_ADDRESS_WIFI_SSID, ssid);
			EEPROM.get(BAK_ADDRESS_WIFI_KEY, pass);
		}else{
			memset(ssid, 0, sizeof(ssid));
			strcpy(ssid, "mks1");
			memset(pass, 0, sizeof(pass));
			strcpy(pass, "makerbase");
		}
		
		currentState = OperatingState::Client;
		package_net_para();
		Serial.write(uart_send_package, uart_send_size);		
		transfer_state = TRANSFER_READY;
		delay(1000);
		WiFi.mode(WIFI_STA);
		WiFi.disconnect();
		delay(1000);
		EEPROM.get(BAK_ADDRESS_MANUAL_IP_FLAG, manual_valid);
		if(manual_valid == 0xa){
			EEPROM.get(BAK_ADDRESS_MANUAL_IP, ip_static);
			EEPROM.get(BAK_ADDRESS_MANUAL_MASK, subnet_static);
			EEPROM.get(BAK_ADDRESS_MANUAL_GATEWAY, gateway_staic);
			EEPROM.get(BAK_ADDRESS_MANUAL_DNS, dns_static);

			WiFi.config(ip_static, gateway_staic, subnet_static, dns_static, (uint32_t)0x00000000);
		}

		WiFi.begin(ssid, pass);

		while (WiFi.status() != WL_CONNECTED){
			if(get_printer_reply() > 0){
				esp_data_parser((char *)uart_rcv_package, uart_rcv_index);
			}
			uart_rcv_index = 0;
			package_net_para();
		
			Serial.write(uart_send_package, uart_send_size);
			delay(500);
			
			failcount++;
			
			if (failcount > MAX_WIFI_FAIL){
			  delay(100);
			  return false;
			}
			do_transfer();
			
	 	}
  	}else{
		if(eeprom_valid[0] == 0x0a){
			EEPROM.get(BAK_ADDRESS_WIFI_SSID, softApName);
			EEPROM.get(BAK_ADDRESS_WIFI_KEY, softApKey);
		}else{
			String macStr= WiFi.macAddress();
			macStr.replace(":", "");
			
			strcat(softApName, macStr.substring(8).c_str());
			memset(pass, 0, sizeof(pass));
		}
		currentState = OperatingState::AccessPoint;
		
		WiFi.mode(WIFI_AP);
		WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
		if(strlen(softApKey) != 0)
			WiFi.softAP(softApName, softApKey);
		else
			WiFi.softAP(softApName);
	}
  	return true;
}

uint8_t refreshApWeb(){
	wifiConfigHtml = F("<html><head><meta http-equiv='Content-Type' content='text/html;'><title>MKS WIFI</title><style>body{background: #b5ff6a;}.config{margin: 150px auto;width: 600px;height: 600px;overflow: hidden;</style></head>");
	wifiConfigHtml += F("<body><div class='config'></caption><br /><h2>Update</h2>");
	wifiConfigHtml += F("<form method='POST' action='update_sketch' enctype='multipart/form-data'><table border='0'><tr><td>wifi firmware:</td><td><input type='file' name='update' ></td><td><input type='submit' value='update'></td></tr></form>");
	wifiConfigHtml += F("<form method='POST' action='update_spiffs' enctype='multipart/form-data'><tr><td>web view:</td><td><input type='file' name='update' ></td><td><input type='submit' value='update'></td></tr></table></form>");
	wifiConfigHtml += F("<br /><br /><h2>WIFI Configuration</h2><form method='GET' action='update_cfg'><caption><input type='radio' id='wifi_mode_sta' name='wifi_mode' value='wifi_mode_sta' /><label for='wifi_mode_sta'>STA</label><br />");
	wifiConfigHtml += F("<input type='radio' id='wifi_mode_ap' name='wifi_mode' value='wifi_mode_ap' /><label for='wifi_mode_ap'>AP</label><br /><br /><table border='0'><tr><td>");
	wifiConfigHtml += F("WIFI: </td><td><input type='text' id='hidden_ssid' name='hidden_ssid' /></td></tr><tr><td>KEY: </td><td><input type=' PASSWORD_INPUT_TYPE ' id='password' name='password' />");
	wifiConfigHtml += F("</td></tr><tr><td colspan=2 align='right'> <input type='submit' value='config and reboot'></td></tr></table></form></div></body></html>");
	return 0;
}

char hidden_ssid[32] = {0};

void onWifiConfig(){
	refreshApWeb();

	server.on("/", HTTP_GET, []() {
		server.send(200, FPSTR(STR_MIME_TEXT_HTML), wifiConfigHtml);
	});
	server.on("/update_sketch", HTTP_GET, []() {
		server.send(200, FPSTR(STR_MIME_TEXT_HTML), wifiConfigHtml);
	});
	server.on("/update_spiffs", HTTP_GET, []() {
		server.send(200, FPSTR(STR_MIME_TEXT_HTML), wifiConfigHtml);
	});

 
 	server.on("/update_cfg", HTTP_GET, []() {
		if (server.args() <= 0){
			server.send(500, FPSTR(STR_MIME_TEXT_PLAIN), F("Got no data, go back and retry"));
			return;
		}
		for (uint8_t e = 0; e < server.args(); e++) {
			String argument = server.arg(e);
			urldecode(argument);
			if (server.argName(e) == "password") argument.toCharArray(pass, 64);//pass = server.arg(e);
			else if (server.argName(e) == "ssid") argument.toCharArray(ssid, 32);//ssid = server.arg(e);
			else if (server.argName(e) == "hidden_ssid") argument.toCharArray(hidden_ssid, 32);//ssid = server.arg(e);
			else if (server.argName(e) == "wifi_mode") argument.toCharArray(wifi_mode, 15);//ssid = server.arg(e);
			//else if (server.argName(e) == "webhostname") argument.toCharArray(webhostname, 64);
		}

		if(strlen((const char *)hidden_ssid) <= 0){
			server.send(200, FPSTR(STR_MIME_TEXT_HTML), F("<p>wifi parameters error!</p>"));
			return;
		}
		
		if((strcmp(wifi_mode, "wifi_mode_ap") == 0) && (strlen(pass) > 0) && ((strlen(pass) < 8) )){
			server.send(500, FPSTR(STR_MIME_TEXT_PLAIN), F("wifi password length is not correct, go back and retry"));
			return;	
		}else{
			memset(ssid, 0, sizeof(ssid));
			memcpy(ssid, hidden_ssid, sizeof(hidden_ssid));
		}
		
		char valid[1] = {0x0a};
		
		EEPROM.put(BAK_ADDRESS_WIFI_SSID, ssid);
		EEPROM.put(BAK_ADDRESS_WIFI_KEY, pass);
		EEPROM.put(BAK_ADDRESS_WEB_HOST, webhostname);

		EEPROM.put(BAK_ADDRESS_WIFI_MODE, wifi_mode);

		EEPROM.put(BAK_ADDRESS_WIFI_VALID, valid);

		EEPROM.put(BAK_ADDRESS_MANUAL_IP_FLAG, 0xff);
		manual_valid = 0xff;

		EEPROM.commit();
	
		server.send(200, FPSTR(STR_MIME_TEXT_HTML), F("<p>Configure successfully!<br />Please use the new ip to connect again.</p>"));

		delay(300);
		ESP.restart();
	});
}

void StartAccessPoint(){
	delay(5000);
	IPAddress apIP(192, 168, 4, 1);
	WiFi.mode(WIFI_AP);
	WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
	String macStr= WiFi.macAddress();
	macStr.replace(":", "");
	strcat(softApName, macStr.substring(8).c_str());
	WiFi.softAP(softApName);
	onWifiConfig();
	server.begin();
}

void extract_file_item(File dataFile, String fileStr){
//Todo
}
void extract_file_item_cloud(File dataFile, String fileStr){
//Todo
}

void fsHandler(){
	String path = server.uri();

	if(!verification_flag)
	{
		return;
	}
	
	bool addedGz = false;
	File dataFile = SPIFFS.open(path, "r");

	if (!dataFile && !path.endsWith(".gz") && path.length() <= 29)
	{
		// Requested file not found and wasn't a zipped file, so see if we have a zipped version
		path += F(".gz");
		addedGz = true;
		dataFile = SPIFFS.open(path, "r");
	}
	if (!dataFile)
	{
		server.send(404, FPSTR(STR_MIME_APPLICATION_JSON), "{\"err\": \"404: " + server.uri() + " NOT FOUND\"}");
		return;
	}
	// No need to add the file size or encoding headers here because streamFile() does that automatically
	String dataType = FPSTR(STR_MIME_TEXT_PLAIN);
	if (path.endsWith(".html") || path.endsWith(".htm")) dataType = FPSTR(STR_MIME_TEXT_HTML);
	else if (path.endsWith(".css") || path.endsWith(".css.gz")) dataType = F("text/css");
	else if (path.endsWith(".js") || path.endsWith(".js.gz")) dataType = F("application/javascript");
	else if (!addedGz && path.endsWith(".gz")) dataType = F("application/x-gzip");
	else if ( path.endsWith(".png")) dataType = F("application/x-png");
	else if ( path.endsWith(".ico")) dataType = F("image/x-icon");


	server.streamFile(dataFile, dataType);
	dataFile.close();
}

void handleUpload(){
  uint32_t now;
  uint8_t readBuf[1024];

  uint32_t postLength = server.getPostLength();
  String uri = server.uri();
  

  if(uri != NULL){
  	if((transfer_file_flag) || (transfer_state != TRANSFER_IDLE) || (gPrinterInf.print_state != PRINTER_IDLE)){	
		server.send(409, FPSTR(STR_MIME_TEXT_PLAIN), FPSTR("409 Conflict"));	
		return;
	}
		
  	if(server.hasArg((const char *) "X-Filename")){
  		if((transfer_file_flag) || (transfer_state != TRANSFER_IDLE)){
  			server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_IS_BUSY));		
			return;
  		}

		file_fragment = 0;
		rcv_end_flag = false;
		transfer_file_flag = true;
		gFileFifo.reset();
		upload_error = false;
		upload_success = false;

		String FileName = server.arg((const char *) "X-Filename");
		if(package_file_first((char *)FileName.c_str(), (int)postLength) != 0){
			transfer_file_flag = false;
		}

		int wait_tick = 0;
		while(1){
			do_transfer();
			delay(100);
			wait_tick++;
			if(wait_tick > 20){
				if(digitalRead(McuTfrReadyPin) == HIGH){ // STM32 READY SIGNAL
					upload_error = true;		
				}
				break;
			}
			
			int len_get = get_printer_reply();
			if(len_get > 0){
				esp_data_parser((char *)uart_rcv_package, len_get);
				uart_rcv_index = 0;
			}

			if(upload_error){
				break;
			}
		}
		
		if(!upload_error){
			
			now = millis();
			do{
				do_transfer();
				int len = get_printer_reply();
				if(len > 0){
					esp_data_parser((char *)uart_rcv_package, len);
					uart_rcv_index = 0;
				}

				if(upload_error || upload_success){
					break;
				}

				if (postLength != 0){				  
					uint32_t len = gFileFifo.left();

					if (len > postLength){
						 len = postLength;
					}
					
					if(len > sizeof(readBuf)){
						 len = sizeof(readBuf);
					}	
					
					if(len > 0){
						size_t len2 = server.readPostdata(server.client(), readBuf, len);
				
						if (len2 > 0){
							postLength -= len2;
							gFileFifo.push((char *)readBuf, len2);
							now = millis();
						}
					}
				}else{
					rcv_end_flag = true;
					break;
				}
				yield();
			}while (millis() - now < 10000);
		}
		
		if(upload_success || rcv_end_flag ){
			server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_0));	
		}else{
			if(Serial.baudRate() != 115200){
				Serial.flush();
				Serial.begin(115200);
			}
			
			transfer_file_flag = false;
			rcv_end_flag = false;
			transfer_state = TRANSFER_IDLE;
			server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_DATA_RECEIVED));	
		}
		
  	}else{
		server.send(500, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_500_NO_FILENAME_PROVIDED));	
		return;
	}
  }
}


void handleRrUpload() {
}

void cloud_down_file(){
//Todo
}


void cloud_get_file_list()
{
//Todo
}

void cloud_handler()
{
//Todo
}

void urldecode(String &input) { // LAL ^_^
  input.replace("%0A", String('\n'));
  input.replace("%20", " ");
  input.replace("+", " ");
  input.replace("%21", "!");
  input.replace("%22", "\"");
  input.replace("%23", "#");
  input.replace("%24", "$");
  input.replace("%25", "%");
  input.replace("%26", "&");
  input.replace("%27", "\'");
  input.replace("%28", "(");
  input.replace("%29", ")");
  input.replace("%30", "*");
  input.replace("%31", "+");
  input.replace("%2C", ",");
  input.replace("%2E", ".");
  input.replace("%2F", "/");
  input.replace("%2C", ",");
  input.replace("%3A", ":");
  input.replace("%3A", ";");
  input.replace("%3C", "<");
  input.replace("%3D", "=");
  input.replace("%3E", ">");
  input.replace("%3F", "?");
  input.replace("%40", "@");
  input.replace("%5B", "[");
  input.replace("%5C", "\\");
  input.replace("%5D", "]");
  input.replace("%5E", "^");
  input.replace("%5F", "-");
  input.replace("%60", "`");
  input.replace("%7B", "{");
  input.replace("%7D", "}");
}

String urlDecode(const String& text)
{
	String decoded = "";
	char temp[] = "0x00";
	unsigned int len = text.length();
	unsigned int i = 0;
	while (i < len)
	{
		char decodedChar;
		char encodedChar = text.charAt(i++);
		if ((encodedChar == '%') && (i + 1 < len))
		{
			temp[2] = text.charAt(i++);
			temp[3] = text.charAt(i++);

			decodedChar = strtol(temp, NULL, 16);
		}
		else {
			if (encodedChar == '+')
			{
				decodedChar = ' ';
			}
			else {
				decodedChar = encodedChar;  // normal ascii char
			}
		}
		decoded += decodedChar;
	}
	return decoded;
}


void urlencode(String &input) { // LAL ^_^
  input.replace(String('\n'), "%0A");
  input.replace(" " , "+");
  input.replace("!" , "%21");
  input.replace("\"", "%22" );
  input.replace("#" , "%23");
  input.replace("$" , "%24");
  //input.replace("%" , "%25");
  input.replace("&" , "%26");
  input.replace("\'", "%27" );
  input.replace("(" , "%28");
  input.replace(")" , "%29");
  input.replace("*" , "%30");
  input.replace("+" , "%31");
  input.replace("," , "%2C");
  //input.replace("." , "%2E");
  input.replace("/" , "%2F");
  input.replace(":" , "%3A");
  input.replace(";" , "%3A");
  input.replace("<" , "%3C");
  input.replace("=" , "%3D");
  input.replace(">" , "%3E");
  input.replace("?" , "%3F");
  input.replace("@" , "%40");
  input.replace("[" , "%5B");
  input.replace("\\", "%5C" );
  input.replace("]" , "%5D");
  input.replace("^" , "%5E");
  input.replace("-" , "%5F");
  input.replace("`" , "%60");
  input.replace("{" , "%7B");
  input.replace("}" , "%7D");
}

String fileUrlEncode(String str){
    String encodedString="";
    char c;
    char code0;
    char code1;
    for (uint32_t i =0; i < str.length(); i++){
      c=str.charAt(i);
      if (c == ' '){
        encodedString+= '+';
      } else if (isalnum(c)){
        encodedString+=c;
      } else{
        code1=(c & 0xf)+'0';
        if ((c & 0xf) >9){
            code1=(c & 0xf) - 10 + 'A';
        }
        c=(c>>4)&0xf;
        code0=c+'0';
        if (c > 9){
            code0=c - 10 + 'A';
        }
        encodedString+='%';
        encodedString+=code0;
        encodedString+=code1;
        //encodedString+=code2;
      }
      yield();
    }
    return encodedString;
    
}

String fileUrlEncode(char *array)
{
    String encodedString="";
	String str(array);
    char c;
    char code0;
    char code1;
    for (uint32_t i =0; i < str.length(); i++){
      c=str.charAt(i);
      if (c == ' '){
        encodedString+= '+';
      } else if (isalnum(c)){
        encodedString+=c;
      } else{
        code1=(c & 0xf)+'0';
        if ((c & 0xf) >9){
            code1=(c & 0xf) - 10 + 'A';
        }
        c=(c>>4)&0xf;
        code0=c+'0';
        if (c > 9){
            code0=c - 10 + 'A';
        }
        encodedString+='%';
        encodedString+=code0;
        encodedString+=code1;
      }
      yield();
    }
    return encodedString;
    
}

