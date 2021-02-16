#include "taskimpl.h"
#include <fcntl.h>


static Tasklist sleeping;
static int sleepingcounted;
static uvlong nsec(void);
static int startedfdtask;

#ifndef __linux__

#include <sys/poll.h>
enum
{
	MAXFD = 1024
};

static struct pollfd pollfd[MAXFD];
static Task *polltask[MAXFD];
static int npollfd;

void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	
	tasksystem();
	taskname("fdtask");
	for(;;){
		/* let everyone else run */
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("poll");
		if((t=sleeping.head) == nil)
			ms = -1;
		else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
		if(poll(pollfd, npollfd, ms) < 0){
			if(errno == EINTR)
				continue;
			fprint(2, "poll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		for(i=0; i<npollfd; i++){
			while(i < npollfd && pollfd[i].revents){
				taskready(polltask[i]);
				--npollfd;
				pollfd[i] = pollfd[npollfd];
				polltask[i] = polltask[npollfd];
			}
		}
		
		now = nsec();
		while((t=sleeping.head) && now >= t->alarmtime){
			deltask(&sleeping, t);
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

void
fdwait(int fd, int rw)
{
	int bits;

	if(!startedfdtask){
		startedfdtask = 1;
		taskcreate(fdtask, 0, 32768);
	}

	if(npollfd >= MAXFD){
		fprint(2, "too many poll file descriptors\n");
		abort();
	}
	
	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
	bits = 0;
	switch(rw){
	case 'r':
		bits |= POLLIN;
		break;
	case 'w':
		bits |= POLLOUT;
		break;
	}

	polltask[npollfd] = taskrunning;
	pollfd[npollfd].fd = fd;
	pollfd[npollfd].events = bits;
	pollfd[npollfd].revents = 0;
	npollfd++;
	taskswitch();
}

#else /* HAVE_EPOLL */

// Scalable Linux-specific implementation
#include <sys/epoll.h>

static int epfd;

void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	// 变成系统协程
	tasksystem();
	taskname("fdtask");
    struct epoll_event events[1000];
	for(;;){
		/* let everyone else run */
		// 大于0说明还有其他就绪协程可执行，则让给他们执行，否则往下执行
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("epoll");
		// 没有定时事件则一直阻塞
		if((t=sleeping.head) == nil)
			ms = -1;
		else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
        int nevents;
		// 等待事件发生，ms是等待的超时时间
		if((nevents = epoll_wait(epfd, events, 1000, ms)) < 0){
			if(errno == EINTR)
				continue;
			fprint(2, "epoll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		// 事件触发，把对应协程插入就绪队列
		for(i=0; i<nevents; i++){
            taskready((Task *)events[i].data.ptr);
		}

		now = nsec();
		// 处理超时事件
		while((t=sleeping.head) && now >= t->alarmtime){
			deltask(&sleeping, t);
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

// 协程因为等待io需要切换
void
fdwait(int fd, int rw)
{	
	// 是否已经初始化epoll
	if(!startedfdtask){
		startedfdtask = 1;
        epfd = epoll_create(1);
        assert(epfd >= 0);
		// 没有初始化则创建一个协程，做io管理
		taskcreate(fdtask, 0, 32768);
	}

	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
    struct epoll_event ev = {0};
	// 记录事件对应的协程和感兴趣的事件
    ev.data.ptr = taskrunning;
	switch(rw){
	case 'r':
		ev.events |= EPOLLIN | EPOLLPRI;
		break;
	case 'w':
		ev.events |= EPOLLOUT;
		break;
	}

    int r = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    int duped = 0;
    if (r < 0 || errno == EEXIST) {
        duped = 1;
        fd = dup(fd);
        int r = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        assert(r == 0);
    }
	// 切换到其他协程，等待被唤醒
	taskswitch();
	// 唤醒后函数刚才注册的事件
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
    if (duped)
        close(fd);
}

#endif /* HAVE_EPOLL */
// 协程主动睡眠一段时间
uint
taskdelay(uint ms)
{
	uvlong when, now;
	Task *t;
	
	if(!startedfdtask){
		startedfdtask = 1;
#ifdef __linux__
        epfd = epoll_create(1);
        assert(epfd >= 0);
#endif
		taskcreate(fdtask, 0, 32768);
	}

	now = nsec();
	// 睡眠到什么时候
	when = now+(uvlong)ms*1000000;
	// 找到插入的位置，超时时间从小到大，即快到期的在前面
	for(t=sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
		;

	if(t){
		taskrunning->prev = t->prev;
		taskrunning->next = t;
	}else{
		taskrunning->prev = sleeping.tail;
		taskrunning->next = nil;
	}
	
	t = taskrunning;
	// 记录协程的绝对超时时间
	t->alarmtime = when;
	if(t->prev)
		t->prev->next = t;
	else
		sleeping.head = t;
	if(t->next)
		t->next->prev = t;
	else
		sleeping.tail = t;

	if(!t->system && sleepingcounted++ == 0)
		taskcount++;
	// 切换
	taskswitch();
	// 被唤醒后，计算过去的时间间隔
	return (nsec() - now)/1000000;
}


/* Like fdread but always calls fdwait before reading. */
int
fdread1(int fd, void *buf, int n)
{
	int m;
	// 注册到epoll，然后会被切换到其他协程，切换回来的时候，说明io事件触发了，进行读
	do
		fdwait(fd, 'r');
	while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);
	return m;
}

int
fdread(int fd, void *buf, int n)
{
	int m;
	// 非阻塞读，如果不满足则再注册到epoll，参考fdread1
	while((m=read(fd, buf, n)) < 0 && errno == EAGAIN)
		fdwait(fd, 'r');
	return m;
}

// 
int
fdwrite(int fd, void *buf, int n)
{
	int m, tot;
	// 非阻塞写，写失败，则注册等待写事件到epoll，事件触发后，继续写入未写成功的
	for(tot=0; tot<n; tot+=m){
		while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(fd, 'w');
		if(m < 0)
			return m;
		if(m == 0)
			break;
	}
	return tot;
}

// 设置fd为非阻塞
int
fdnoblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

static uvlong
nsec(void)
{
	struct timeval tv;

	if(gettimeofday(&tv, 0) < 0)
		return -1;
	return (uvlong)tv.tv_sec*1000*1000*1000 + tv.tv_usec*1000;
}

