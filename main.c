#include<stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

int main(int argc,char* argv[]) {
	if (argc < 3) {
		printf("./a.out port path\n");
		return -1;
	}
	//忽略SIGPIPE：向已断开的连接发数据时进程不会被杀死，sendfile只会返回错误
	signal(SIGPIPE, SIG_IGN);
	//字符串转化成整形
	unsigned short port = atoi(argv[1]);
	//转换路径
	int ret = chdir(argv[2]);
	if (ret == -1) {
		perror("chdir");
		return -1;
	}
	//初始化监听套接字
	int lfd = InitListenFD(port);
	//启动epoll
	EpollRun(lfd);
	return 0;
}