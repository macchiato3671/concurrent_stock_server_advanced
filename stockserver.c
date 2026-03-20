/* 
 * echoserveri.c - An iterative echo server 
 */ 
/* $begin echoserverimain */
#include "csapp.h"

#define SBUFSIZE 1000
typedef struct item{
	int ID;
	int left_stock;
	int price;
	int readcnt;
	sem_t mutex;
	struct item* left_item;
	struct item* right_item;
} item;
typedef struct {
	int *buf;
	int n;
	int front;
	int rear;
	sem_t mutex;
	sem_t slots;
	sem_t items;
} sbuf_t;
item* root = NULL;
pthread_rwlock_t rwlock;
sbuf_t sbuf;
void echo(int connfd);
void *thread(void *varpg);
void insert(item* compare, int ID, int left_stock, int price);
item* makeInit(int ID, int left_stock, int price);
void traverse(item* node, char* buf);
void sell_check(item* node, int ID, int left_stock);
int buy_check(item* node, int ID, int left_stock);
void apply_result(item* node, FILE* fp);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);

int main(int argc, char **argv) 
{
    int listenfd, stockId, stockNum, stockPrice, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;  /* Enough space for any address */  //line:netp:echoserveri:sockaddrstorage
    char client_hostname[MAXLINE], client_port[MAXLINE];
    pthread_t tid;
    FILE* fp;

    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(0);
    }

    fp = Fopen("stock.txt", "r");
    while(fscanf(fp, "%d %d %d\n", &stockId, &stockNum, &stockPrice) != EOF){
    	if(root == NULL){
		root = makeInit(stockId, stockNum, stockPrice);
	}
	else{
		insert(root, stockId, stockNum, stockPrice);
	}
    }
    Fclose(fp);
    pthread_rwlock_init(&rwlock,NULL);

    listenfd = Open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);
    for (int i = 0 ; i < SBUFSIZE ; i++){
    	Pthread_create(&tid, NULL, thread, NULL);
    }
    while (1) {
	clientlen = sizeof(struct sockaddr_storage); 
	connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, 
                    client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_hostname, client_port);
     	sbuf_insert(&sbuf, connfd);
    }
    exit(0);
}

void *thread(void *vargp){
	char* token, *stock_num, *stock_id;
	char buf[MAXLINE];
	int n, stockNum, stockId;
	rio_t rio;

	int connfd = sbuf_remove(&sbuf);
	Pthread_detach(pthread_self());
	Rio_readinitb(&rio ,connfd);
	//서버 동작
	while((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0){
		printf("Server received %d bytes on fd %d\n", n,connfd);
		buf[n-1] = '\0';
		token = strtok(buf," ");
		if(!strcmp(token, "show")){
			buf[0] = '\0';
			pthread_rwlock_rdlock(&rwlock);
			traverse(root, buf);
			pthread_rwlock_unlock(&rwlock);
			strcat(buf, "\0");
		}
		else if(!strcmp(token, "buy")){
			token = strtok(NULL, " ");
                        stock_id = token;
                        token = strtok(NULL, " ");
                        stock_num = token;
                        stockId = atoi(stock_id);
                        stockNum = atoi(stock_num);
			if(buy_check(root, stockId, stockNum)){
				strcpy(buf, "[buy] success\n");
			}
			else{
				strcpy(buf, "Not enough left stock\n");
			}	
		}
		else if(!strcmp(token, "sell")){
			token = strtok(NULL, " ");
			stock_id = token;
			token = strtok(NULL, " ");
			stock_num = token;
			stockId = atoi(stock_id);
			stockNum = atoi(stock_num);
			sell_check(root, stockId, stockNum);
			strcpy(buf, "[sell] success\n");
		}
			
		if(!strcmp(token, "exit")){
			Close(connfd);
		}
		else{
			Rio_writen(connfd, buf, MAXLINE);
		}
		buf[0] = '\0';
	}
	Close(connfd);
	pthread_rwlock_wrlock(&rwlock);
	FILE* fp = Fopen("stock.txt", "w");
	apply_result(root, fp);
	Fclose(fp);
	pthread_rwlock_unlock(&rwlock);
	return NULL;
}

void insert(item* compare, int ID, int left_stock, int price){
	item* temp;
	
	if(compare->ID > ID){
		if(compare->left_item == NULL){
			temp = makeInit(ID, left_stock, price);
			compare->left_item = temp;
		}
		else{
			insert(compare->left_item, ID, left_stock, price);
		}
	}
	else{
		if(compare->right_item == NULL){
			temp = makeInit(ID, left_stock, price);
			compare->right_item = temp;
		}
		else{
			insert(compare->right_item, ID, left_stock, price);
		}
	}
}

item* makeInit(int ID, int left_stock, int price){
	item* temp = (item*)malloc(sizeof(item));
	
	temp->ID = ID;
	temp->left_stock = left_stock;
	temp->price = price;
	temp->readcnt = 0;
	temp->left_item = NULL;
	temp->right_item = NULL;

	return temp;
}

void traverse(item* node, char* buf){
	if(node == NULL){
		return;
	}
	char temp[10];
	sprintf(temp, "%d", node->ID);
	strcat(buf,temp);
	strcat(buf, " ");

	sprintf(temp, "%d", node->left_stock);
	strcat(buf, temp);
	strcat(buf, " ");

	sprintf(temp, "%d", node->price);
	strcat(buf, temp);
	strcat(buf, "\n");
	traverse(node->left_item, buf);
	traverse(node->right_item, buf);
}

void sell_check(item* node, int ID, int left_stock){
	if(node == NULL){
		return;
	}
	if(node->ID == ID){
		pthread_rwlock_wrlock(&rwlock);
		node->left_stock += left_stock;
		pthread_rwlock_unlock(&rwlock);
		return;
	}
	
	if(ID < node->ID){
		sell_check(node->left_item, ID, left_stock);
	}
	else{
		sell_check(node->right_item, ID, left_stock);
	}
}

int buy_check(item* node, int ID, int left_stock){
	int n1 = 0, n2 = 0;

	if(node == NULL){
		return 0;
	}

	if(node->ID == ID){
		pthread_rwlock_wrlock(&rwlock);
		if(node->left_stock >= left_stock){
			node->left_stock -= left_stock;
			pthread_rwlock_unlock(&rwlock);
			return 1;
		}
		else{
			pthread_rwlock_unlock(&rwlock);
			return 0;
		}
	}

	if(ID < node->ID){
		n1 = buy_check(node->left_item, ID, left_stock);
	}
	else{
		n2 = buy_check(node->right_item, ID, left_stock);
	}

	if(n1 | n2){
		return 1;
	}
	else{
		return 0;
	}
}

void apply_result(item* node, FILE* fp){
	if(node == NULL){
		return;
	}
	fprintf(fp, "%d %d %d\n", node->ID, node->left_stock, node->price);
	apply_result(node->left_item, fp);
	apply_result(node->right_item, fp);
}

void sbuf_init(sbuf_t *sp, int n){
	sp->buf = Calloc(n, sizeof(int));
	sp->n = n;
	sp->front = sp->rear = 0;
	Sem_init(&sp->mutex, 0, 1);
	Sem_init(&sp->slots, 0, n);
	Sem_init(&sp->items, 0, 0);
}

void sbuf_deinit(sbuf_t *sp){
	Free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item){
	P(&sp->slots);
	P(&sp->mutex);
	sp->buf[(++sp->rear)%(sp->n)] = item;
	V(&sp->mutex);
	V(&sp->items);
}

int sbuf_remove(sbuf_t *sp){
	int item;
	P(&sp->items);
	P(&sp->mutex);
	item = sp->buf[(++sp->front)%(sp->n)];
	V(&sp->mutex);
	V(&sp->slots);
	return item;
}
/* $end echoserverimain */
