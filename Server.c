#include<stdio.h>
#include"Server.h"
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>


int InitListenFD(unsigned short port) {
	//创建监听fd
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		perror("socket");
		return -1;
	}
	//设置端口复用
	int opt = 1;
	int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if(ret==-1){
		perror("setsockopt");
		return -1;
	}
	//绑定端口
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) {
		perror("bind");
		return -1;
	}
	//设置监听
	ret = listen(lfd, 128);
	if (ret == -1) {
		perror("listen");
		return -1;
	}
	return lfd;
}
int EpollRun(int lfd) {
	//创建epoll树
	int epfd = epoll_create(2);
	if (epfd == -1) {
		perror("epoll_create");
		return -1;
	}
	//将监听文件描述符放入epoll树
	struct epoll_event ev;
	ev.data.fd = lfd;
	ev.events = EPOLLIN;
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1) {
		perror("epoll_ctl");
		return -1;
	}
	//处于就绪状态数组
	struct epoll_event evs[1024];//epoll树中检测出来的个数就算超过1024也没关系，因为是循环检测
	int size = sizeof(evs) / sizeof(struct epoll_event);
	//阻塞循环检测
	while (1) {
		int num = epoll_wait(epfd, &evs, size, -1);
		if (num == -1) {
			perror("epoll_wait");
			return -1;
		}
		for (int i = 0; i < num; i++) {
			int fd = evs[i].data.fd;
			//接受连接
			if (fd == lfd) {
				//与客户端连接并将获得的通信文件描述符添加到epll数上面
				AcceptClient(lfd,epfd);
			}
			//进行通信
			else {
				//接收客户端http请求消息
				RecieveHttpRequest(fd,epfd);
			}
		}
	}
	return 0;
}
int AcceptClient(int lfd,int epfd) {
	int cfd = accept(lfd, NULL, NULL);
	if (cfd == -1) {
		perror("accept");
		return -1;
	}
	//设置边沿非阻塞
	int flag = fcntl(cfd, F_GETFL);
	flag |= O_NONBLOCK;
	fcntl(cfd, F_SETFL, flag);
	//cfd添加到epoll树
	struct epoll_event ev;
	ev.data.fd = cfd;
	ev.events = EPOLLIN | EPOLLET;
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1) {
		perror("epoll_ctl");
		return -1;
	}
	return 0;
}
int RecieveHttpRequest(int fd,int epfd) {
	char buf[4096]={0};
	char temp[1024] = { 0 };
	int count = 0;
	int len = read(fd, temp, sizeof(temp));
	while (len > 0) {
		if (count + len < sizeof(buf)) {
			memcpy(buf + count, temp, len);
			count += len;
		}
		else {
			printf("buf is full,request to large\n");
			break;
		}
		len=read(fd, temp, sizeof(temp));
	}
	//缓冲区数据读完了，可以解析http
	if (len == -1 && errno == EAGAIN) {
		int size = strlen(buf);
		//解析请求行之前先把请求行提取出来
		for (int i = 0; i < size; i++) {
			if (buf[i] == '\r') {
				buf[i] = '\0';
				break;
			}
		}
		//解析http请求行
		ParesRequestLine(buf,fd);
	}
	else if (len == 0) {
		//客户端断开连接
		printf("client cut connecct\n");
		//解除epoll树上对应通信文件描述符
		int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		if (ret == -1) {
			perror("epoll_ctl");
		}
		close(fd);
	}
	else {
		perror("read");
	}
	return 0;
}
int ParesRequestLine(const char* line,int cfd) {
	// 解析请求行 get /xxx/1.jpg http/1.1
	char method[12];
	char path[1024];
	//用sscanf进行分割
	sscanf(line, "%[^ ] %[^ ]", method, path);
	printf("method=%s,path=%s\n", method, path);
	//忽略大小写比较
	if (strcasecmp(method, "get") != 0) {
		printf("refuse http method not get\n");
		return -1;
	}
	//对于path进行转换，转换成基于当前进程下的相对路径
	// 因为程序员一般会把客户端要查找的文件放在当前进程工作目录下，
	//当前进程工作目录需要在main函数里切换
	char* temp = NULL;
	if (strcmp(path,"/")==0) {
		temp = "./";
	}
	else {
		temp = path + 1;
	}
	//查询文件信息
	struct stat st;
	int ret = stat(temp, &st);
	if (ret == -1) {
		//文件不存在,回复404
		printf("404\n");
		SendHeadMsg(cfd, 404, "Not Found", GetFileType(".html"), -1);
		//404.html这个文件需要我们自己加在main函数切换的目录文件夹里面
		SendFile("404.html", cfd);
		return 0;
	}
	//判断路径文件是不是目录
	if (S_ISDIR(st.st_mode)) {
		//把目录内容发送给客户端
		printf("dir\n");
		SendHeadMsg(cfd, 200, "OK", GetFileType(".html"), st.st_size);
		SendDir(temp, cfd);
	}
	else {
		//把文件内容发送给客户端
		//通过http响应发送文件
		printf("file\n");
		SendHeadMsg(cfd, 200, "OK", GetFileType(temp), st.st_size);
		SendFile(temp,cfd);
	}
	return 0;
}
int SendFile(char* filename,int cfd) {
	//获取文件描述符
	int fd = open(filename, O_RDONLY);
	//断言文件描述符 fd 必须大于 0；如果 `fd <= 0`，直接崩溃报错
	assert(fd > 0);
	printf("sendfile...\n");
	/*char buf[1024];
	while (1) {
		int len = read(fd, buf, sizeof(buf));
		if (len > 0) {
			send(cfd, buf, len, 0);
			usleep(10);//睡眠10微妙，给客户端处理数据的时间，防止发送错乱
		}
		else if (len == 0) {
			//读完了
			break;
		}
		else {
			perror("read");
		}

	}*/
	int size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	sendfile(cfd, fd, NULL, size);
	close(fd);
	return 0;
}
int SendHeadMsg(int cfd, int status, const char* descrip, char* type, int length) {
	printf("SendHeadMsg\n");
	char buf[4096];
	//状态行
	sprintf(buf, "http/1.1 %d %s\r\n", status, descrip);
	//响应头+空行
		sprintf(buf + strlen(buf), "Content-Type: %s\r\n", type);
		sprintf(buf + strlen(buf), "Content-Length: %d\r\n\r\n", length);
	send(cfd, buf, strlen(buf), 0);
	printf("SendHeadMsg finish\n");
	return 0;
}

//查找响应头里面type
const char* GetFileType(const char* name)
{
	// a.jpg a.mp4 a.html
	// 自右向左查找‘.’字符, 如不存在返回NULL
	const char* dot = strrchr(name, '.');
	if (dot == NULL)
		return "text/plain; charset=utf-8";

	if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
		return "text/html; charset=utf-8";
	else if (strcasecmp(dot, ".css") == 0)
		return "text/css; charset=utf-8";
	else if (strcasecmp(dot, ".js") == 0)
		return "application/javascript; charset=utf-8";
	else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
		return "image/jpeg";
	else if (strcasecmp(dot, ".png") == 0)
		return "image/png";
	else if (strcasecmp(dot, ".gif") == 0)
		return "image/gif";
	else if (strcasecmp(dot, ".ico") == 0)
		return "image/x-icon";
	else if (strcasecmp(dot, ".svg") == 0)
		return "image/svg+xml";
	else if (strcasecmp(dot, ".mp4") == 0)
		return "video/mp4";
	else if (strcasecmp(dot, ".mov") == 0)
		return "video/quicktime";
	else if (strcasecmp(dot, ".avi") == 0)
		return "video/x-msvideo";
	else if (strcasecmp(dot, ".mpeg") == 0 || strcasecmp(dot, ".mpe") == 0)
		return "video/mpeg";
	else if (strcasecmp(dot, ".midi") == 0 || strcasecmp(dot, ".mid") == 0)
		return "audio/midi";
	else if (strcasecmp(dot, ".ogg") == 0)
		return "audio/ogg";
	else if (strcasecmp(dot, ".wav") == 0)
		return "audio/wav";
	else if (strcasecmp(dot, ".au") == 0)
		return "audio/basic";
	else if (strcasecmp(dot, ".vrml") == 0 || strcasecmp(dot, ".wrl") == 0)
		return "model/vrml";
	else if (strcasecmp(dot, ".pac") == 0)
		return "application/x-ns-proxy-autoconfig";
	else if (strcasecmp(dot, ".mp3") == 0)
		return "audio/mpeg";
	else if (strcasecmp(dot, ".txt") == 0)
		return "text/plain; charset=utf-8";
	else if (strcasecmp(dot, ".json") == 0)
		return "application/json; charset=utf-8";
	else if (strcasecmp(dot, ".pdf") == 0)
		return "application/pdf";

	// 未知后缀
	return "text/plain; charset=utf-8";
}
/*
<html>
	<head>
		<title>test</title>
	</head>
	<body>
		<table>
			<tr>
				<td></td>
				<td></td>
			</tr>
			<tr>
				<td></td>
				<td></td>
			</tr>
		</table>
	</body>
</html>
*/
int SendDir(char* dirname, int cfd) {
	//拼接html网页头部
	printf("senddir\n");
	char buf[4096] = {0};
	sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirname);
	//namelist（传出参数）指向指针数组 struct dirent* tmp[]，数组每一个元素都是一个指针，指向dirname下面的条目（文件或文件夹）
	struct dirent** namelist;
	int num = scandir(dirname, &namelist, NULL, alphasort);
	//遍历目录下每一个条目，根据文件属性判断是文件还是目录，并进行html拼接
	for (int i = 0; i < num; i++) {
		char subPath[1024] = {0};
		char* name = namelist[i]->d_name;
		//跳过 . 和 .. 这两个特殊目录条目，不显示在网页上
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			free(namelist[i]);
			continue;
		}
		//拼接路径
		sprintf(subPath, "%s/%s", dirname, name);
		struct stat st;
		int ret = stat(subPath, &st);
		//条目是目录，拼接html，能够点击连接并跳转,%s后面要加/代表目录
		if (S_ISDIR(st.st_mode)) {
			// a标签 <a href="">name</a>
			sprintf(buf + strlen(buf), 
				"<tr><td><a href=\"%s/\">%s</a></td><td>%d</td></tr>", name, name, st.st_size);

		}
		//条目是文件
		else {
			sprintf(buf + strlen(buf),
				"<tr><td><a href=\"%s\">%s</a></td><td>%d</td></tr>", name, name, st.st_size);
		}
		//发送数据
		send(cfd, buf, strlen(buf), 0);
		//重置buf
		memset(buf, 0, sizeof(buf));
		//数组里每一个 namelist[i]指向的 struct dirent 对象，也单独 malloc 出来,要释放
		free(namelist[i]);
	}
	//拼接结束html
	sprintf(buf, "</table></body></html>");
	send(cfd, buf, strlen(buf), 0);
	//namelist是一个指针数组malloc一块内存，用来存放一堆struct dirent*指针
	free(namelist);
	return 0;
}

