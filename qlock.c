#include "taskimpl.h"

/*
 * locking
 */
// 加锁
static int
_qlock(QLock *l, int block)
{	
	// 锁没有持有者，则置当前协程为持有者，直接返回，1表示加锁成功
	if(l->owner == nil){
		l->owner = taskrunning;
		return 1;
	}
	// 非阻塞，则直接返回，0表示加锁失败
	if(!block)
		return 0;
	// 插入等待锁队列
	addtask(&l->waiting, taskrunning);
	taskstate("qlock");
	// 切换到其他协程
	taskswitch();
	// 切换回来时，如果持有锁的协程不是当前协程，则异常退出，因为只有持有锁才会被切换回来，见unqlock
	if(l->owner != taskrunning){
		fprint(2, "qlock: owner=%p self=%p oops\n", l->owner, taskrunning);
		abort();
	}
	return 1;
}

// 阻塞式加锁
void
qlock(QLock *l)
{
	_qlock(l, 1);
}

// 非阻塞式加锁
int
canqlock(QLock *l)
{
	return _qlock(l, 0);
}

// 释放锁
void
qunlock(QLock *l)
{
	Task *ready;
	// 锁并没有持有者，异常退出
	if(l->owner == 0){
		fprint(2, "qunlock: owner=0\n");
		abort();
	}
	// 如果还有协程在等待该锁，则置为持有者，并且从等待队列中删除，然后修改状态为就绪并加入就绪队列
	if((l->owner = ready = l->waiting.head) != nil){
		deltask(&l->waiting, ready);
		taskready(ready);
	}
}

// 加读锁
static int
_rlock(RWLock *l, int block)
{	
	/*
		没有正在写并且没有等待写，则加锁成功，并且读者数加一
	*/
	if(l->writer == nil && l->wwaiting.head == nil){
		l->readers++;
		return 1;
	}
	// 非阻塞则直接返回
	if(!block)
		return 0;
	// 插入等待读队列
	addtask(&l->rwaiting, taskrunning);
	taskstate("rlock");
	// 切换上下文
	taskswitch();
	// 切换回来了，说明加锁成功
	return 1;
}

// 阻塞时加读锁
void
rlock(RWLock *l)
{
	_rlock(l, 1);
}

// 非阻塞式加读锁
int
canrlock(RWLock *l)
{
	return _rlock(l, 0);
}

// 加写锁
static int
_wlock(RWLock *l, int block)
{	
	// 没有正在写并且没有正在读，则加锁成功，并置写者为当前协程
	if(l->writer == nil && l->readers == 0){
		l->writer = taskrunning;
		return 1;
	}
	// 非阻塞则直接返回
	if(!block)
		return 0;
	// 加入等待写队列
	addtask(&l->wwaiting, taskrunning);
	taskstate("wlock");
	// 切换
	taskswitch();
	// 切换回来说明拿到锁了
	return 1;
}

// 阻塞时加写锁
void
wlock(RWLock *l)
{
	_wlock(l, 1);
}

// 非阻塞时加写锁 
int
canwlock(RWLock *l)
{
	return _wlock(l, 0);
}

// 释放读锁
void
runlock(RWLock *l)
{
	Task *t;
	// 读者减一，如果等于0并且有等待写的协程，则队列第一个协程持有该锁
	if(--l->readers == 0 && (t = l->wwaiting.head) != nil){
		deltask(&l->wwaiting, t);
		l->writer = t;
		taskready(t);
	}
}

// 释放写锁
void
wunlock(RWLock *l)
{
	Task *t;
	// 没有正在写，异常退出
	if(l->writer == nil){
		fprint(2, "wunlock: not locked\n");
		abort();
	}
	// 置空，没有协程正在写
	l->writer = nil;
	// 有正在读，异常退出，写的时候，是无法读的
	if(l->readers != 0){
		fprint(2, "wunlock: readers\n");
		abort();
	}
	// 释放写锁时，优先让读者持有锁，因为读者可以共享持有锁，提高并发
	// 读可以共享，把等待读的协程都加入就绪队列，并持有锁
	while((t = l->rwaiting.head) != nil){
		deltask(&l->rwaiting, t);
		l->readers++;
		taskready(t);
	}
	// 释放写锁时，如果又没有读者，并且有等待写的协程，则队列的第一个等待写的协程持有锁
	if(l->readers == 0 && (t = l->wwaiting.head) != nil){
		deltask(&l->wwaiting, t);
		l->writer = t;
		taskready(t);
	}
}
