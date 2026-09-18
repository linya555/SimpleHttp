#pragma once
//初始化监听套接字
int InitListenFD(unsigned short port);
//启动epoll
int EpollRun(int lfd);
//接收连接
void* AcceptClient(void* arg);
//接收客户端http请求消息
void* RecieveHttpRequest(void* arg);
//解析http
int ParesRequestLine(const char* line, int cfd);
//把文件内容发送给客户端（http响应第四部分 响应数据）
int SendFile(char* filename, int cfd);
//组织http响应的前三个部分（状态行，响应头，空行）
int SendHeadMsg(int cfd,int status,const char* descrip,char* type,int length);
const char* GetFileType(const char* name);
//发送目录列表网页
int SendDir(char* dirname, int cfd);
//中文解码
int hexToDec(char c);
void decodeMsg(char* to, char* from);
