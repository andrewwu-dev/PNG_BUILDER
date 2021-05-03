/* Libraries */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <search.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include "zutil.h"
#include "crc.h"
#include "shm_stack.h"

#define ECE252_HEADER "X-Ece252-Fragment: "
#define SEM_PROC 1
#define BUF_SIZE 1048576
#define BUF_INC  524288
#define TOTAL_IMG_STRIPS 50
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

/* Structs */
typedef struct {
    unsigned int width;
    unsigned int height;
} Dimension;

typedef struct {
    unsigned int length;
    unsigned char type[4];
    unsigned char crc[4];
    int data_shmid;
    unsigned char *data;
} Chunk;

typedef struct RECV_BUF {
    int buf_shmid;
    char *buf;
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
    /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef struct img_strips_count {
    int count;
} img_strips_count;

/* Global Pointers */
unsigned char gp_PNG_signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
unsigned char gp_IHDR_signature[4] = {73, 72, 68, 82};
unsigned char gp_IDAT_signature[4] = {73, 68, 65, 84};
unsigned char gp_IEND_signature[4] = {73, 69, 78, 68};
int *gp_img_strips;
int *gp_img_strips_count;
U8 *gp_IDAT_data_buf; /* Stores uncompressed png data to concat */
int gp_IDAT_data_buf_len;
Dimension gp_png_dimension;

pthread_mutex_t gp_mutex;
sem_t *gp_spaces;
sem_t *gp_items;
struct int_stack *gp_stack;

/* Function Declarations */
int* init(int b);
void clean(int *shmids);

void hex_dimension_to_int(Dimension **dimension, const unsigned char *data);
void process_png_strip(const char *data, int seq_num);
void build_png();
unsigned char *create_IHDR_data();
unsigned char *create_IDAT_data();
unsigned long create_crc(Chunk *chunk);
void add_scanline_to_buf(U8 *scanline, int length);
void write_chunk(FILE *png_fp, Chunk *chunk);
Chunk *read_chunk(int curr_pos, const char *data);

RECV_BUF *curl(int machine_number, int img_number, int part_number);
size_t write_callback(char *png_data, size_t size, size_t number_items, void *userdata);
size_t header_callback(char *header, size_t size, size_t number_items, void *userdata);

int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);

void produce(int start, int end, int img_number);
void consume(int start, int end, int x);

/* Initializer */
int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <B> <P> <C> <X> <N>\n", argv[0]);
        exit(1);
    }

    int b = atoi(argv[1]);
    int p = atoi(argv[2]);
    int c = atoi(argv[3]);
    int x = atoi(argv[4]);
    int n = atoi(argv[5]);
    double times[2];
    struct timeval tv;
    pid_t p_pids[p];
    pid_t c_pids[c];

    int *shmids = init(b);

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;

    int m = 50/p;
    for (int i = 0;i < p;i++) {
        p_pids[i] = fork();
        if (p_pids[i] == 0 ) { /* child proc */
            if (i == p - 1) {
                produce(i*m, 49, n);
            } else {
                produce(i*m, (i+1)*m-1, n);
            }
            break;
        }
    }

    m = 50/c;
    for (int i = 0;i < c;i++) {
        c_pids[i] = fork();
        if (c_pids[i] == 0 ) { /* child proc */
            if (i == c - 1) {
                consume(i*m, 49, x);
            } else {
                consume(i*m, (i+1)*m-1, x);
            }
            break;
        }
    }

    for (int i = 0;i < p;i++) {
        waitpid(p_pids[i], NULL, 0);
    }
    for (int i = 0;i < c;i++) {
        waitpid(c_pids[i], NULL, 0);
    }

    for (int i = 0; i < TOTAL_IMG_STRIPS; i++) {
 	Chunk *temp = shmat(gp_img_strips[i], NULL, 0);	
        U8 *data = shmat(temp->data_shmid, NULL, 0);

	add_scanline_to_buf(data, temp->length);

	shmdt(data);
	shmctl(temp->data_shmid, IPC_RMID, NULL);
	shmdt(temp);
	shmctl(gp_img_strips[i], IPC_RMID, NULL);
    }

    build_png();

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }

    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("Parent pid = %d: total execution time is %.6lf seconds\n", getpid(),  times[1] - times[0]);

    clean(shmids);

    return 0;
}

int* init(int b) {
    int *shmids = malloc(6*sizeof(int));
    shmids[0] = shmget(IPC_PRIVATE, sizeof_shm_stack(b), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_stack = shmat(shmids[0], NULL, 0);
    shmids[1] = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_spaces = shmat(shmids[1], NULL, 0);
    shmids[2] = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_items = shmat(shmids[2], NULL, 0);
    shmids[3] = shmget(IPC_PRIVATE, sizeof(pthread_mutex_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_mutex = *(pthread_mutex_t *)shmat(shmids[3], NULL, 0);
    shmids[4] = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_img_strips_count = shmat(shmids[4], NULL, 0);
    shmids[5] = shmget(IPC_PRIVATE, 50 * sizeof(Chunk), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    gp_img_strips = shmat(shmids[5], NULL, 0);

    // init
    memset(gp_img_strips_count, 0, 1);
    gp_IDAT_data_buf_len = 0;
    gp_png_dimension.width = 400;
    gp_png_dimension.height = 300;
    init_shm_stack(gp_stack, b);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gp_mutex, &attr);
    sem_init(gp_spaces, SEM_PROC, b);
    sem_init(gp_items, SEM_PROC, 0);

    return shmids;
}

void clean(int *shmids) {
    shmdt(gp_img_strips);
    shmdt(gp_img_strips_count);
    shmdt(&gp_mutex);
    shmdt(gp_items);
    shmdt(gp_spaces);
    shmdt(gp_stack);
    for (int i = 0;i < 6;i++) {\
        shmctl(shmids[i], IPC_RMID, NULL);
    }

    pthread_mutex_destroy(&gp_mutex);
    sem_destroy(gp_spaces);
    sem_destroy(gp_items);

    free(shmids);
    curl_global_cleanup();
}

void produce(int start, int end, int img_number) {
    RECV_BUF *recv_buf = NULL;
    int machine_number = 1;

    for (int i = start;i <= end;i++) {
        recv_buf = curl(machine_number, img_number, i);
	int shmid = shmget(IPC_PRIVATE, sizeof(RECV_BUF), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
        RECV_BUF *temp = shmat(shmid, NULL, 0);
        temp->seq = recv_buf->seq;
        temp->max_size = recv_buf->max_size;
        temp->size = recv_buf->size;

        int shmid2 = shmget(IPC_PRIVATE, recv_buf->size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
        char *temp2 = shmat(shmid2, NULL, 0);
        memcpy(temp2, recv_buf->buf, recv_buf->size);
        temp->buf_shmid = shmid2;
	shmdt(temp2);

        recv_buf_cleanup(recv_buf);
        free(recv_buf);

        sem_wait(gp_spaces);
        pthread_mutex_lock(&gp_mutex);
        push(gp_stack, shmid);
        pthread_mutex_unlock(&gp_mutex);
        sem_post(gp_items);


        shmdt(temp);
        machine_number += 1;
        if (machine_number > 3) {
            machine_number = 1;
        }
    }

    exit(0);
}

void consume(int start, int end, int x) {
    while(1) {
        sem_wait(gp_items);
        pthread_mutex_lock(&gp_mutex);
        int shmid;
        RECV_BUF *recv_buf = NULL;
        if (TOTAL_IMG_STRIPS - *gp_img_strips_count > 0) {
	  sleep(x/1000);
          pop(gp_stack, &shmid);
          recv_buf = shmat(shmid, NULL, 0);
	}
	pthread_mutex_unlock(&gp_mutex);
        sem_post(gp_spaces);
                
        if (recv_buf != NULL) {
          char* buf = shmat(recv_buf->buf_shmid, NULL, 0);

	  process_png_strip(buf, recv_buf->seq);

          // Delete buffer shared memory
          shmdt(buf);
          shmctl(recv_buf->buf_shmid, IPC_RMID, NULL);
          // Delete recv_buf shared memory
          shmdt(recv_buf);
          shmctl(shmid, IPC_RMID, NULL);
	} 
   	if (TOTAL_IMG_STRIPS - *gp_img_strips_count <= 0) {
	   sem_post(gp_items);
	   break;
	}
    }

    exit(0);
}

Chunk *read_chunk(int curr_pos, const char *data) {
    Chunk *chunk = malloc(sizeof(Chunk));
    unsigned int data_length;

    memcpy(&data_length, &data[curr_pos], 4);
    curr_pos += 4;
    chunk->length = ntohl(data_length);
    memcpy(chunk->type, &data[curr_pos], 4);
    curr_pos += 4;
    if (chunk->length > 0) {
        chunk->data = malloc(chunk->length);
        memcpy(chunk->data, &data[curr_pos], chunk->length);
        curr_pos += chunk->length;
    }
    memcpy(chunk->crc, &data[curr_pos], 4);
    return chunk;
}

void write_chunk(FILE *png_fp, Chunk *chunk) {
    unsigned int length = htonl(chunk->length);
    fwrite(&length, sizeof(chunk->length), 1, png_fp);
    fwrite(chunk->type, sizeof(chunk->type), 1, png_fp);
    if (chunk->length > 0) {
        fwrite(chunk->data, sizeof(char), chunk->length, png_fp);
    }
    fwrite(chunk->crc, sizeof(chunk->crc), 1, png_fp);
}

Dimension *get_dimension_from_IHDR(Chunk *ihdr_chunk) {
    unsigned char data[8];
    // Copy over dimension data
    memcpy(&data, ihdr_chunk->data, 8);
    /* width x height */
    Dimension *dimension = malloc(sizeof(Dimension));
    hex_dimension_to_int(&dimension, data);
    return dimension;
}

void hex_dimension_to_int(Dimension **dimension, const unsigned char *data) {
    char temp[64];
    sprintf(temp, "%02X%02X%02X%02X", data[0], data[1], data[2], data[3]);
    sscanf(temp, "%x", &(*dimension)->width);
    sprintf(temp, "%02X%02X%02X%02X", data[4], data[5], data[6], data[7]);
    sscanf(temp, "%x", &(*dimension)->height);
}

void add_scanline_to_buf(U8 *scanline, int length) {
    if (gp_IDAT_data_buf_len == 0) {
        gp_IDAT_data_buf = malloc(length);
    } else {
        int new_length = length + gp_IDAT_data_buf_len;
        U8 *temp_buf = realloc(gp_IDAT_data_buf, new_length * sizeof(U8));
        if (temp_buf) { /* Successful allocation */
            gp_IDAT_data_buf = temp_buf;
        } else {
            printf("Ran out of memory\n");
            exit(-2);
        }
    }
    /* Append scanline to the end of data buffer */
    memcpy(gp_IDAT_data_buf + gp_IDAT_data_buf_len, scanline, length * sizeof(U8));
    gp_IDAT_data_buf_len += length;
}

void process_png_strip(const char *data, int seq_num) {
    int cur_pos = 0;

    /* Skip to beginning of IHDR chunk */
    cur_pos += 8;
    Chunk *chunk = read_chunk(cur_pos, data); /* IHDR chunk */
    cur_pos += (chunk->length + 12); // 4 + 4 + data length + 4
    Dimension *dimension = get_dimension_from_IHDR(chunk);

    /* Process IDAT chunk */
    free(chunk->data);
    free(chunk);
    chunk = read_chunk(cur_pos, data);
    /* Uncrompress IDAT data*/
    U64 len_inf = 0;
    U8 temp_buf[dimension->height * (dimension->width * 4 + 1)];
    int result = mem_inf(temp_buf, &len_inf, chunk->data, chunk->length);
    if (result != 0) {
        printf("Inflation(De-Compression) of IDAT failed\n");
        exit(-1);
    }


    chunk->length = len_inf;
    free(chunk->data);
    chunk->data = malloc(len_inf);
    memcpy(chunk->data, temp_buf, len_inf);

    int chunk_shmid = shmget(IPC_PRIVATE, sizeof(Chunk), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    Chunk *temp = shmat(chunk_shmid, NULL, 0);
    memcpy(temp, chunk, sizeof(Chunk));
    
    int data_shmid = shmget(IPC_PRIVATE, chunk->length, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    U8 *temp2 = shmat(data_shmid, NULL, 0);
    memcpy(temp2, chunk->data, chunk->length);
    temp->data_shmid = data_shmid;

    pthread_mutex_lock(&gp_mutex);
    *gp_img_strips_count += 1;
    memcpy(&gp_img_strips[seq_num], &chunk_shmid, sizeof(int));
    pthread_mutex_unlock(&gp_mutex);

    shmdt(temp2);
    shmdt(temp);
    free(chunk);
    free(dimension);
}

unsigned char *create_IHDR_data() {
    unsigned char *data = malloc(13);
    unsigned int width = htonl(gp_png_dimension.width);
    unsigned int height = htonl(gp_png_dimension.height);
    unsigned char meta_IHDR[5] = {8, 6, 0, 0, 0};

    memcpy(data, &width, sizeof(int));
    memcpy(data+4, &height, sizeof(int));
    memcpy(data+8, &meta_IHDR, sizeof(meta_IHDR));
    return data;
}

unsigned char *create_IDAT_data() {
    U64 len_def = 0;
    U8 def_buf[gp_IDAT_data_buf_len];
    int result = mem_def(def_buf, &len_def, gp_IDAT_data_buf, gp_IDAT_data_buf_len, Z_DEFAULT_COMPRESSION);
    if (result != 0) {
        printf("Deflation(Compression) of IDAT failed\n");
        exit(-1);
    }

    free(gp_IDAT_data_buf);
    unsigned char *data = malloc(len_def);
    memcpy(data, def_buf, len_def);
    gp_IDAT_data_buf_len = len_def;
    return data;
}

unsigned long create_crc(Chunk *chunk) {
    int data_len = chunk->length + 4;
    unsigned char *data = malloc(data_len);

    memcpy(data, chunk->type, sizeof(chunk->type));
    if (chunk->length > 0) {
        memcpy(data + 4, chunk->data, chunk->length);
    }
    unsigned long crc_value = htonl(crc(data, data_len));
    free(data);
    return crc_value;
}

void build_png() {
    FILE *png_fp = fopen("all.png", "wb");
    if (png_fp == NULL) {
        printf("Unable to create PNG file\n");
        exit(-3);
    }
    /* PNG header */
    fwrite(gp_PNG_signature, sizeof(char), 8, png_fp);

    /* IHDR */
    Chunk *chunk = malloc(sizeof(Chunk));
    chunk->length = 13;
    memcpy(chunk->type, gp_IHDR_signature, sizeof(gp_IHDR_signature));
    chunk->data = create_IHDR_data();
    unsigned long crc_val = create_crc(chunk);
    memcpy(chunk->crc, &crc_val, 4 * sizeof(char));
    write_chunk(png_fp, chunk);

    /* IDAT */
    free(chunk->data);
    free(chunk);
    chunk = malloc(sizeof(Chunk));
    memcpy(chunk->type, gp_IDAT_signature, sizeof(gp_IDAT_signature));
    chunk->data = create_IDAT_data();
    chunk->length = gp_IDAT_data_buf_len;
    crc_val = create_crc(chunk);
    memcpy(chunk->crc, &crc_val, 4 * sizeof(char));
    write_chunk(png_fp, chunk);

    /* IEND */
    free(chunk->data);
    free(chunk);
    chunk = malloc(sizeof(Chunk));
    chunk->length = 0;
    memcpy(chunk->type, gp_IEND_signature, sizeof(gp_IEND_signature));
    crc_val = create_crc(chunk);
    memcpy(chunk->crc, &crc_val, 4 * sizeof(char));
    write_chunk(png_fp, chunk);

    free(chunk);
    fclose(png_fp);
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size) {
    void *p = NULL;

    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
        return 2;
    }

    ptr->buf = p;
    ptr->buf_shmid = -1;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr) {
    if (ptr == NULL) {
        return 1;
    }

    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

RECV_BUF *curl(int machine_number, int img_number, int part_number) {
    char url[55];
    CURL *request;
    CURLcode response;

    RECV_BUF *recv_buf = malloc(sizeof(RECV_BUF));
    recv_buf_init(recv_buf, BUF_SIZE);

    response = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (response != CURLE_OK) {
        fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(response));
        return NULL;
    }

    request = curl_easy_init();
    snprintf(url, 55, "http://ece252-%d.uwaterloo.ca:2530/image?img=%d&part=%d", machine_number, img_number, part_number);
    url[54] = '\0';

    if (request) {
        curl_easy_setopt(request, CURLOPT_URL, url);
        curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(request, CURLOPT_WRITEDATA, recv_buf);
        curl_easy_setopt(request, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(request, CURLOPT_HEADERDATA, recv_buf);
        curl_easy_setopt(request, CURLOPT_USERAGENT, "libcurl-agent/1.0");

        response = curl_easy_perform(request);
        if (response != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(response));
            return NULL;
        }

        curl_easy_cleanup(request);
    }

    return recv_buf;
}

size_t write_callback(char *png_data, size_t size, size_t number_items, void *userdata) {
    size_t realsize = size * number_items;
    RECV_BUF *p = userdata;

    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + MAX(BUF_INC, realsize + 1);
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc");
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, png_data, realsize);
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

size_t header_callback(char *header, size_t size, size_t number_items, void *userdata) {
    int realsize = size * number_items;
    RECV_BUF *p = userdata;

    if (realsize > strlen(ECE252_HEADER) && strncmp(header, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {
        p->seq = atoi(header + strlen(ECE252_HEADER));
    }

    return realsize;
}
