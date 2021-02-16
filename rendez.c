#include "taskimpl.h"

/*
 * sleep and wakeup
 */
// 把当前协程投入睡眠
void
tasksleep(Rendez *r)
{
	// 插入等待队列
	addtask(&r->waiting, taskrunning);
	// 持有锁则先释放
	if(r->l)
		qunlock(r->l);
	taskstate("sleep");
	// 切换协程
	taskswitch();
	// 切换回来时加锁
	if(r->l)
		qlock(r->l);
}

// 唤醒协程，all代表是否唤醒所有
static int
_taskwakeup(Rendez *r, int all)
{
	int i;
	Task *t;

	for(i=0;; i++){
		// 不是唤醒所有，则在唤醒第一个协程后就跳出循环
		if(i==1 && !all)
			break;
		// 没有可唤醒的协程则跳出
		if((t = r->waiting.head) == nil)
			break;
		// 移出等待队列，插入就绪队列
		deltask(&r->waiting, t);
		taskready(t);
	}
	return i;
}

int
taskwakeup(Rendez *r)
{
	return _taskwakeup(r, 0);
}

int
taskwakeupall(Rendez *r)
{
	return _taskwakeup(r, 1);
}

