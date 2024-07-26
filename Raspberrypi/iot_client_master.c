#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <mysql/mysql.h>

#define BUF_SIZE 100
#define NAME_SIZE 20
#define ARR_CNT 6
#define WINDOW_CLOSE 0
#define WINDOW_OPEN 1

void *send_msg(void *arg);
void *recv_msg(void *arg);
void error_handling(char *msg);
int query(MYSQL *conn, char *sql_cmd);

char name[NAME_SIZE] = "[Default]";
char msg[BUF_SIZE];

// MP_CONFIG 테이블
int is_activate = 1;			// 활성화 여부 (0: 비활성화, 1: 활성화)
int window_state = WINDOW_OPEN; // 창문 상태 (0: 닫힘, 1: 열림)
int water_threshold = 100;		// 빗물 감지 임계치
int air_threshold = 100;		// 대기질 감지 임계치

int main(int argc, char *argv[])
{
	int sock;
	struct sockaddr_in serv_addr;
	pthread_t snd_thread, rcv_thread, mysql_thread;
	void *thread_return;

	// 인자 확인
	if (argc != 4)
	{
		printf("Usage : %s <IP> <port> <name>\n", argv[0]);
		exit(1);
	}

	sprintf(name, "%s", argv[3]);

	// 소켓 생성
	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		error_handling("socket() error");

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
	serv_addr.sin_port = htons(atoi(argv[2]));

	// 서버에 연결
	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
		error_handling("connect() error");

	// 로그인 정보 전송
	sprintf(msg, "[%s:PASSWD]", name);
	write(sock, msg, strlen(msg));

	pthread_create(&rcv_thread, NULL, recv_msg, (void *)&sock);
	pthread_create(&snd_thread, NULL, send_msg, (void *)&sock);

	pthread_join(snd_thread, &thread_return);
	pthread_join(rcv_thread, &thread_return);

	close(sock);
	return 0;
}

/**
 * 메시지 전송 쓰레드
 */
void *send_msg(void *arg)
{
	int *sock = (int *)arg;
	int str_len;
	int ret;
	fd_set initset, newset;
	struct timeval tv;
	char name_msg[NAME_SIZE + BUF_SIZE + 2];

	FD_ZERO(&initset);
	FD_SET(STDIN_FILENO, &initset);

	fputs("Input a message! [ID]msg (Default ID:ALLMSG)\n", stdout);
	while (1)
	{
		memset(msg, 0, sizeof(msg));
		name_msg[0] = '\0';
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		newset = initset;
		ret = select(STDIN_FILENO + 1, &newset, NULL, NULL, &tv);
		if (FD_ISSET(STDIN_FILENO, &newset))
		{
			fgets(msg, BUF_SIZE, stdin);
			// 종료
			if (!strncmp(msg, "quit\n", 5))
			{
				*sock = -1;
				return NULL;
			}
			// 전체 메시지
			else if (msg[0] != '[')
			{
				strcat(name_msg, "[ALLMSG]");
				strcat(name_msg, msg);
			}
			// 특정 ID 메시지
			else
			{
				strcpy(name_msg, msg);
			}

			// 메시지 전송
			if (write(*sock, name_msg, strlen(name_msg)) <= 0)
			{
				*sock = -1;
				return NULL;
			}
		}
		if (ret == 0)
		{
			if (*sock == -1)
				return NULL;
		}
	}
}

/**
 * 메시지 수신 쓰레드
 */
void *recv_msg(void *arg)
{
	// MySQL 정보
	MYSQL *conn;
	MYSQL_ROW sqlrow;
	int res;
	char sql_cmd[200] = {0};
	char *host = "localhost";
	char *user = "iot";
	char *pass = "pwiot";
	char *dbname = "iotdb";

	// 수신 정보
	int *sock = (int *)arg;
	int i;
	char *pToken;
	char *pArray[ARR_CNT] = {0};

	char send_buffer[NAME_SIZE + BUF_SIZE + 1];
	char recv_buffer[NAME_SIZE + BUF_SIZE + 1];
	int recv_length;

	// MP_SENSOR 테이블
	int sensor_hum;	  // 습도
	int sensor_temp;  // 온도
	int sensor_water; // 빗물
	int sensor_air;	  // 대기질

	// MP_LOG 테이블
	int log_value; // 창문 상태
	int log_user;  // 사용자 ID

	conn = mysql_init(NULL);

	// MySQL 연결
	puts("MYSQL startup");
	if (!(mysql_real_connect(conn, host, user, pass, dbname, 0, NULL, 0)))
	{
		fprintf(stderr, "ERROR : %s[%d]\n", mysql_error(conn), mysql_errno(conn));
		exit(1);
	}
	else
		printf("Connection Successful!\n\n");

	while (1)
	{
		memset(recv_buffer, 0x0, sizeof(recv_buffer));
		recv_length = read(*sock, recv_buffer, NAME_SIZE + BUF_SIZE);
		if (recv_length <= 0)
		{
			*sock = -1;
			return NULL;
		}
		recv_buffer[recv_length] = 0;
		fputs(recv_buffer, stdout);

		// 메시지 토큰화
		pToken = strtok(recv_buffer, "[:@]");
		i = 0;
		while (pToken != NULL)
		{
			pArray[i] = pToken;
			if (++i >= ARR_CNT)
				break;
			pToken = strtok(NULL, "[:@]");
		}
		if (pArray[i - 1][strlen(pArray[i - 1]) - 1] == '\n')
		{
			pArray[i - 1][strlen(pArray[i - 1]) - 1] = '\0';
		}

		// 센서 값 수신
		// 2: HUMI, 3: TEMP, 4: WATER, 5: AIR
		if (!strcmp(pArray[1], "SENSOR") && (i == 6))
		{
			sensor_hum = atoi(pArray[2]);
			sensor_temp = atoi(pArray[3]);
			sensor_water = atoi(pArray[4]);
			sensor_air = atoi(pArray[5]);

			// 조건에 따라 모터 제어
			if (is_activate)
			{
				// 빗물 감지
				if (sensor_water > water_threshold && window_state == WINDOW_OPEN)
				{
					window_state = WINDOW_CLOSE;
					strcpy(send_buffer, "[MP_MOTOR]MOTOR@CLOSE\n");
					write(*sock, send_buffer, strlen(send_buffer));
					fputs("Water: closed\n", stdout);

					sprintf(sql_cmd, "insert into mp_log(date, time, value, user) values(now(), now(), %d, 'WATER')", WINDOW_CLOSE);
					res = query(conn, sql_cmd);
				}
				// 대기질 감지
				if (sensor_air > air_threshold && window_state == WINDOW_OPEN)
				{
					window_state = WINDOW_CLOSE;
					strcpy(send_buffer, "[MP_MOTOR]MOTOR@CLOSE\n");
					write(*sock, send_buffer, strlen(send_buffer));
					fputs("Air: closed\n", stdout);

					sprintf(sql_cmd, "insert into mp_log(date, time, value, user) values(now(), now(), %d, 'AIR')", WINDOW_CLOSE);
					res = query(conn, sql_cmd);
				}
			}

			// MP_SENSOR 테이블에 삽입
			sprintf(sql_cmd, "insert into mp_sensor(date, time, hum, temp, water, air) values(now(), now(), %d, %d, %d, %d)", sensor_hum, sensor_temp, sensor_water, sensor_air);
			res = query(conn, sql_cmd);
		}
		// 설정 변경
		else if (!strcmp(pArray[1], "SET") && (i == 4))
		{
			if (!strcmp(pArray[2], "ACTIVATE"))
			{
				is_activate = atoi(pArray[3]) == 0 ? 0 : 1;
				fputs("Activate changed\n", stdout);
				sprintf(sql_cmd, "update mp_config set value=%d where name='activate'", is_activate);
			}
			else if (!strcmp(pArray[2], "WATER"))
			{
				water_threshold = atoi(pArray[3]);
				fputs("Water threshold changed\n", stdout);
				sprintf(sql_cmd, "update mp_config set value=%d where name='water'", water_threshold);
			}
			else if (!strcmp(pArray[2], "AIR"))
			{
				air_threshold = atoi(pArray[3]);
				fputs("Air threshold changed\n", stdout);
				sprintf(sql_cmd, "update mp_config set value=%d where name='air'", air_threshold);
			}
			else
			{
				continue;
			}
			res = query(conn, sql_cmd);
		}
		// 모터 수동 제어
		else if (!strcmp(pArray[1], "MOTOR") && (i == 3))
		{
			if (!strcmp(pArray[2], "OPEN"))
			{
				window_state = WINDOW_OPEN;
				strcpy(send_buffer, "[MP_MOTOR]MOTOR@OPEN\n");
				write(*sock, send_buffer, strlen(send_buffer));
				fputs("Manual open\n", stdout);
				sprintf(sql_cmd, "insert into mp_log(date, time, value, user) values(now(), now(), %d, '%s')", WINDOW_OPEN, pArray[0]);
			}
			else if (!strcmp(pArray[2], "CLOSE"))
			{
				window_state = WINDOW_CLOSE;
				strcpy(send_buffer, "[MP_MOTOR]MOTOR@CLOSE\n");
				write(*sock, send_buffer, strlen(send_buffer));
				fputs("Manual close\n", stdout);
				sprintf(sql_cmd, "insert into mp_log(date, time, value, user) values(now(), now(), %d, '%s')", WINDOW_CLOSE, pArray[0]);
			}
			else
			{
				continue;
			}
			res = query(conn, sql_cmd);
		}
		else if (!strcmp(pArray[1], "TEST") && (i == 3)) {
			if (!strcmp(pArray[2], "AIR")) {
				strcpy(send_buffer, "[MP_MASTER]SENSOR@40@27@10@300\n");
				write(*sock, send_buffer, strlen(send_buffer));
			}
		}
		else
		{
			continue;
		}

		// MySQL 쿼리
		// res = mysql_query(conn, sql_cmd);
		// if (!res)
		// 	printf("inserted %lu rows\n", (unsigned long)mysql_affected_rows(conn));
		// else
		// 	fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
	}
	mysql_close(conn);
}

int query(MYSQL *conn, char *sql_cmd)
{
	int res;

	res = mysql_query(conn, sql_cmd);
	if (!res)
		printf("inserted %lu rows\n", (unsigned long)mysql_affected_rows(conn));
	else
		fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));

	return res;
}

/**
 * 에러 처리 함수
 */
void error_handling(char *msg)
{
	fputs(msg, stderr);
	fputc('\n', stderr);
	exit(1);
}
