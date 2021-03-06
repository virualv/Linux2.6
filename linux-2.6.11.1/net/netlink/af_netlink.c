/*
 * NETLINK      Kernel-user communication protocol.
 *
 * 		Authors:	Alan Cox <alan@redhat.com>
 * 				Alexey Kuznetsov <kuznet@ms2.inr.ac.ru>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 * 
 * Tue Jun 26 14:36:48 MEST 2001 Herbert "herp" Rosmanith
 *                               added netlink_proto_exit
 * Tue Jan 22 18:32:44 BRST 2002 Arnaldo C. de Melo <acme@conectiva.com.br>
 * 				 use nlk_sk, as sk->protinfo is on a diet 8)
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/smp_lock.h>
#include <linux/notifier.h>
#include <linux/security.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/scm.h>

#define Nprintk(a...)

#if defined(CONFIG_NETLINK_DEV) || defined(CONFIG_NETLINK_DEV_MODULE)
#define NL_EMULATE_DEV
#endif

struct netlink_opt
{
	u32			pid;                /* 接收消息的进程pid */
	unsigned int		groups;       /* 记录套接字在哪个多播组 */
	u32			dst_pid;
	unsigned int		dst_groups;
	unsigned long		state;
	int			(*handler)(int unit, struct sk_buff *skb);
	wait_queue_head_t	wait;
	struct netlink_callback	*cb;
	spinlock_t		cb_lock;
	void			(*data_ready)(struct sock *sk, int bytes);  /* netlink数据准备好的回调函数 */
};

#define nlk_sk(__sk) ((struct netlink_opt *)(__sk)->sk_protinfo)

/* netlink进程pid的hash结构 */
struct nl_pid_hash {
	struct hlist_head *table;   /* 通过pid来hash出一个对应的hash链表的链首 */
	unsigned long rehash_time;

	unsigned int mask;
	unsigned int shift;

	unsigned int entries;
	unsigned int max_shift;

	u32 rnd;
};

struct netlink_table {
	struct nl_pid_hash hash;  /* 根据pid进行hash的netlink sock链表，相当于客户端链表 */
	struct hlist_head mc_list;  /* 多播的sock链表 */
	unsigned int nl_nonroot;  /* 监听者标记 ，表示是否需要root权限 */
};

/* 内核中所有的netlink套接字都存储在一个全局的hash表当中，
  * 不同的sock先是根据协议类型来hash出对应的索引
  */
static struct netlink_table *nl_table;

/* 定义等待队列 */
static DECLARE_WAIT_QUEUE_HEAD(nl_table_wait);

static int netlink_dump(struct sock *sk);
static void netlink_destroy_callback(struct netlink_callback *cb);

/* 定义nl_table的锁 */
static DEFINE_RWLOCK(nl_table_lock);
/* 初始化为0 */
static atomic_t nl_table_users = ATOMIC_INIT(0);

static struct notifier_block *netlink_chain;

/* 通过hash函数来获取pid对应的hash链首 */
static struct hlist_head *nl_pid_hashfn(struct nl_pid_hash *hash, u32 pid)
{
	return &hash->table[jhash_1word(pid, hash->rnd) & hash->mask];
}

static void netlink_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->sk_receive_queue);

	if (!sock_flag(sk, SOCK_DEAD)) {
		printk("Freeing alive netlink socket %p\n", sk);
		return;
	}
	BUG_TRAP(!atomic_read(&sk->sk_rmem_alloc));
	BUG_TRAP(!atomic_read(&sk->sk_wmem_alloc));
	BUG_TRAP(!nlk_sk(sk)->cb);

	kfree(nlk_sk(sk));
}

/* This lock without WQ_FLAG_EXCLUSIVE is good on UP and it is _very_ bad on SMP.
 * Look, when several writers sleep and reader wakes them up, all but one
 * immediately hit write lock and grab all the cpus. Exclusive sleep solves
 * this, _but_ remember, it adds useless work on UP machines.
 */

/* 函数返回时表示nl_table_users的值为0 */
static void netlink_table_grab(void)
{
	write_lock_bh(&nl_table_lock);

	if (atomic_read(&nl_table_users)) {
                /* 形成当前进程的等待队列 */
		DECLARE_WAITQUEUE(wait, current);
                /* 添加到等待队列中 */
		add_wait_queue_exclusive(&nl_table_wait, &wait);
		for(;;) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			if (atomic_read(&nl_table_users) == 0)
				break;
			write_unlock_bh(&nl_table_lock);
			schedule();
			write_lock_bh(&nl_table_lock);
		}

		__set_current_state(TASK_RUNNING);
                /* 从队列中移除 */
		remove_wait_queue(&nl_table_wait, &wait);
	}
}

static __inline__ void netlink_table_ungrab(void)
{
	write_unlock_bh(&nl_table_lock);
	wake_up(&nl_table_wait);
}

/* 增加引用计数 */
static __inline__ void
netlink_lock_table(void)
{
	/* read_lock() synchronizes us to netlink_table_grab */

	read_lock(&nl_table_lock);
	atomic_inc(&nl_table_users);
	read_unlock(&nl_table_lock);
}

/* 减小引用计数，当nl_table_users为0时，唤醒等待在nl_table_wait上的等待队列 */
static __inline__ void
netlink_unlock_table(void)
{
	if (atomic_dec_and_test(&nl_table_users))
		wake_up(&nl_table_wait);
}


/* 根据协议和进程pid来获取全局hash表中的sock结构 */
static __inline__ struct sock *netlink_lookup(int protocol, u32 pid)
{
	struct nl_pid_hash *hash = &nl_table[protocol].hash;
	struct hlist_head *head;
	struct sock *sk;
	struct hlist_node *node;

	read_lock(&nl_table_lock);
	head = nl_pid_hashfn(hash, pid);
	sk_for_each(sk, node, head) {
		if (nlk_sk(sk)->pid == pid) {
			sock_hold(sk);
			goto found;
		}
	}
	sk = NULL;
found:
	read_unlock(&nl_table_lock);
	return sk;
}

static inline struct hlist_head *nl_pid_hash_alloc(size_t size)
{
	if (size <= PAGE_SIZE)
		return kmalloc(size, GFP_ATOMIC);
	else
		return (struct hlist_head *)
			__get_free_pages(GFP_ATOMIC, get_order(size));
}

static inline void nl_pid_hash_free(struct hlist_head *table, size_t size)
{
	if (size <= PAGE_SIZE)
		kfree(table);
	else
		free_pages((unsigned long)table, get_order(size));
}

static int nl_pid_hash_rehash(struct nl_pid_hash *hash, int grow)
{
	unsigned int omask, mask, shift;
	size_t osize, size;
	struct hlist_head *otable, *table;
	int i;

	omask = mask = hash->mask;
	osize = size = (mask + 1) * sizeof(*table);
	shift = hash->shift;

	if (grow) {
		if (++shift > hash->max_shift)
			return 0;
		mask = mask * 2 + 1;
		size *= 2;
	}

	table = nl_pid_hash_alloc(size);
	if (!table)
		return 0;

	memset(table, 0, size);
	otable = hash->table;
	hash->table = table;
	hash->mask = mask;
	hash->shift = shift;
	get_random_bytes(&hash->rnd, sizeof(hash->rnd));

	for (i = 0; i <= omask; i++) {
		struct sock *sk;
		struct hlist_node *node, *tmp;

		sk_for_each_safe(sk, node, tmp, &otable[i])
			__sk_add_node(sk, nl_pid_hashfn(hash, nlk_sk(sk)->pid));
	}

	nl_pid_hash_free(otable, osize);
	hash->rehash_time = jiffies + 10 * 60 * HZ;
	return 1;
}

static inline int nl_pid_hash_dilute(struct nl_pid_hash *hash, int len)
{
	int avg = hash->entries >> hash->shift;

	if (unlikely(avg > 1) && nl_pid_hash_rehash(hash, 1))
		return 1;

	if (unlikely(len > avg) && time_after(jiffies, hash->rehash_time)) {
		nl_pid_hash_rehash(hash, 0);
		return 1;
	}

	return 0;
}

static struct proto_ops netlink_ops;

/* 指定进程接收消息，
  * sk表示绑定的套接字 
  * pid为接收消息的进程pid 
  * 绑定成功之后，就是将sk变量hash到nl_table当中  
  */
static int netlink_insert(struct sock *sk, u32 pid)
{
	struct nl_pid_hash *hash = &nl_table[sk->sk_protocol].hash;
	struct hlist_head *head;
	int err = -EADDRINUSE;        /* 首先初始化已经被使用 */
	struct sock *osk;
	struct hlist_node *node;
	int len;

	netlink_table_grab();
	head = nl_pid_hashfn(hash, pid);
	len = 0;
        /* 如果同一个进程创建多个netlink套接字 */
	sk_for_each(osk, node, head) {
		if (nlk_sk(osk)->pid == pid)
			break;
		len++;
	}
        /* 如果在链表当中找到了同一个进程pid */
	if (node)
		goto err;

	err = -EBUSY;
	if (nlk_sk(sk)->pid)
		goto err;

	err = -ENOMEM;
	if (BITS_PER_LONG > 32 && unlikely(hash->entries >= UINT_MAX))
		goto err;

	if (len && nl_pid_hash_dilute(hash, len))
		head = nl_pid_hashfn(hash, pid);
	hash->entries++;
        /* 插入之后，就设置插入的pid，如果指定了就是用指定的，
          如果设置为0，则查找一个合适的 */
	nlk_sk(sk)->pid = pid;
        /* 将sk添加到head对应的链表当中 */
	sk_add_node(sk, head);
	err = 0;

err:
	netlink_table_ungrab();
	return err;
}

static void netlink_remove(struct sock *sk)
{
	netlink_table_grab();
	nl_table[sk->sk_protocol].hash.entries--;
	sk_del_node_init(sk);
        /* 如果自己在组播里面，则从相应的组播里面删除 */
	if (nlk_sk(sk)->groups)
		__sk_del_bind_node(sk);
	netlink_table_ungrab();
}

/* 创建netlink协议族套接字 */
static int netlink_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct netlink_opt *nlk;

	sock->state = SS_UNCONNECTED;

        /* netlink只支持SOCK_RAW和SOCK_DGRAM类型 */
	if (sock->type != SOCK_RAW && sock->type != SOCK_DGRAM)
		return -ESOCKTNOSUPPORT;

	if (protocol<0 || protocol >= MAX_LINKS)
		return -EPROTONOSUPPORT;

	sock->ops = &netlink_ops;

	sk = sk_alloc(PF_NETLINK, GFP_KERNEL, 1, NULL);
	if (!sk)
		return -ENOMEM;

	sock_init_data(sock,sk);
	sk_set_owner(sk, THIS_MODULE);

	nlk = sk->sk_protinfo = kmalloc(sizeof(*nlk), GFP_KERNEL);
	if (!nlk) {
		sk_free(sk);
		return -ENOMEM;
	}
        /* 对数据清0 */
	memset(nlk, 0, sizeof(*nlk));

	spin_lock_init(&nlk->cb_lock);
	init_waitqueue_head(&nlk->wait);
	sk->sk_destruct = netlink_sock_destruct;

	sk->sk_protocol = protocol;
	return 0;
}

/* netlink套接字释放 */
static int netlink_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk;

	if (!sk)
		return 0;

	netlink_remove(sk);
	nlk = nlk_sk(sk);

	spin_lock(&nlk->cb_lock);
	if (nlk->cb) {
		nlk->cb->done(nlk->cb);
		netlink_destroy_callback(nlk->cb);
		nlk->cb = NULL;
		__sock_put(sk);
	}
	spin_unlock(&nlk->cb_lock);

	/* OK. Socket is unlinked, and, therefore,
	   no new packets will arrive */

	sock_orphan(sk);
	sock->sk = NULL;
	wake_up_interruptible_all(&nlk->wait);

	skb_queue_purge(&sk->sk_write_queue);

        /* 如果套接字有想其他进程发送消息或其他组播组发送消息，
          * 则在释放的时候需要告诉其他进程或组播组 
          */
	if (nlk->pid && !nlk->groups) {
		struct netlink_notify n = {
						.protocol = sk->sk_protocol,
						.pid = nlk->pid,
					  };
		notifier_call_chain(&netlink_chain, NETLINK_URELEASE, &n);
	}	
	
	sock_put(sk);
	return 0;
}

static int netlink_autobind(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct nl_pid_hash *hash = &nl_table[sk->sk_protocol].hash;
	struct hlist_head *head;
	struct sock *osk;
	struct hlist_node *node;
	s32 pid = current->pid;
	int err;
	static s32 rover = -4097;

retry:
	cond_resched();
	netlink_table_grab();
        /* 根据pid来hash出链表头部，注意此时的pid为当前进程的pid */
	head = nl_pid_hashfn(hash, pid);
        /* 注意如果很多进程在发送消息的时候，将自己的pid设置为0
          * 那么在自动绑定的时候，则会通过pid来hash的链表中进行查找， 
          * 如果pid已经被绑定，则继续寻找一个没有被绑定的，如果没有被绑定 
          * 则直接使用  
          */
	sk_for_each(osk, node, head) {
		if (nlk_sk(osk)->pid == pid) {
			/* Bind collision, search negative pid values. */
			pid = rover--;
			if (rover > -4097)
				rover = -4097;
			netlink_table_ungrab();
			goto retry;
		}
	}
	netlink_table_ungrab();

        /* 然后将找到的合适pid插入到全局hash表当中 */
	err = netlink_insert(sk, pid);
	if (err == -EADDRINUSE)
		goto retry;
        /* 自动绑定之后，不进入任何组播组 */
	nlk_sk(sk)->groups = 0;
	return 0;
}

static inline int netlink_capable(struct socket *sock, unsigned int flag) 
{ 
	return (nl_table[sock->sk->sk_protocol].nl_nonroot & flag) ||
	       capable(CAP_NET_ADMIN);
} 

/* 绑定netlink协议族套接字 */
static int netlink_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk = nlk_sk(sk);
	struct sockaddr_nl *nladdr = (struct sockaddr_nl *)addr;
	int err;
	
	if (nladdr->nl_family != AF_NETLINK)
		return -EINVAL;

	/* Only superuser is allowed to listen multicasts */
        /* 如果要加入多播组，则需要管理员权限 */
	if (nladdr->nl_groups && !netlink_capable(sock, NL_NONROOT_RECV))
		return -EPERM;

        /* 在调用netlink的socket系统调用后，nlk的所有数据都被清0，
          * 如果多次绑定，且设置的进程pid不一样，则返回无效错误 
          */
	if (nlk->pid) {
		if (nladdr->nl_pid != nlk->pid)
			return -EINVAL;
	} else {
                /* 第一次绑定，如果绑定的进程pid大于0，则直接插入全局列表
                  * 否则自动绑定 
                  */
		err = nladdr->nl_pid ?
			netlink_insert(sk, nladdr->nl_pid) :
			netlink_autobind(sock);
		if (err)
			return err;
	}

        /* 如果不进入任何多播组就直接返回 */
	if (!nladdr->nl_groups && !nlk->groups)
		return 0;

	netlink_table_grab();
        /* 如果再次绑定的时候不进入多播组，则将自己从多播组中删除 */
	if (nlk->groups && !nladdr->nl_groups)
		__sk_del_bind_node(sk);
        /* 将自己添加到多播组当中 */
	else if (!nlk->groups && nladdr->nl_groups)
		sk_add_bind_node(sk, &nl_table[sk->sk_protocol].mc_list);
        /* 设置多播组 */
	nlk->groups = nladdr->nl_groups;
	netlink_table_ungrab();

	return 0;
}

static int netlink_connect(struct socket *sock, struct sockaddr *addr,
			   int alen, int flags)
{
	int err = 0;
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk = nlk_sk(sk);
	struct sockaddr_nl *nladdr=(struct sockaddr_nl*)addr;

	if (addr->sa_family == AF_UNSPEC) {
		sk->sk_state	= NETLINK_UNCONNECTED;
		nlk->dst_pid	= 0;
		nlk->dst_groups = 0;
		return 0;
	}
	if (addr->sa_family != AF_NETLINK)
		return -EINVAL;

	/* Only superuser is allowed to send multicasts */
	if (nladdr->nl_groups && !netlink_capable(sock, NL_NONROOT_SEND))
		return -EPERM;

	if (!nlk->pid)
		err = netlink_autobind(sock);

	if (err == 0) {
		sk->sk_state	= NETLINK_CONNECTED;
		nlk->dst_pid 	= nladdr->nl_pid;
		nlk->dst_groups = nladdr->nl_groups;
	}

	return err;
}

static int netlink_getname(struct socket *sock, struct sockaddr *addr, int *addr_len, int peer)
{
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk = nlk_sk(sk);
	struct sockaddr_nl *nladdr=(struct sockaddr_nl *)addr;
	
	nladdr->nl_family = AF_NETLINK;
	nladdr->nl_pad = 0;
	*addr_len = sizeof(*nladdr);

	if (peer) {
		nladdr->nl_pid = nlk->dst_pid;
		nladdr->nl_groups = nlk->dst_groups;
	} else {
		nladdr->nl_pid = nlk->pid;
		nladdr->nl_groups = nlk->groups;
	}
	return 0;
}

static void netlink_overrun(struct sock *sk)
{
	if (!test_and_set_bit(0, &nlk_sk(sk)->state)) {
		sk->sk_err = ENOBUFS;
		sk->sk_error_report(sk);
	}
}

/* 返回目的进程对应的sock结构 */
static struct sock *netlink_getsockbypid(struct sock *ssk, u32 pid)
{
	int protocol = ssk->sk_protocol;
	struct sock *sock;
	struct netlink_opt *nlk;

        /* 寻找目的进程的sock */
	sock = netlink_lookup(protocol, pid);
	if (!sock)
		return ERR_PTR(-ECONNREFUSED);

	/* Don't bother queuing skb if kernel socket has no input function */
	nlk = nlk_sk(sock);
	if ((nlk->pid == 0 && !nlk->data_ready) ||
	    (sock->sk_state == NETLINK_CONNECTED &&
	     nlk->dst_pid != nlk_sk(ssk)->pid)) {
		sock_put(sock);
		return ERR_PTR(-ECONNREFUSED);
	}
	return sock;
}

struct sock *netlink_getsockbyfilp(struct file *filp)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct socket *socket;
	struct sock *sock;

	if (!inode->i_sock || !(socket = SOCKET_I(inode)))
		return ERR_PTR(-ENOTSOCK);

	sock = socket->sk;
	if (sock->sk_family != AF_NETLINK)
		return ERR_PTR(-EINVAL);

	sock_hold(sock);
	return sock;
}

/*
 * Attach a skb to a netlink socket.
 * The caller must hold a reference to the destination socket. On error, the
 * reference is dropped. The skb is not send to the destination, just all
 * all error checks are performed and memory in the queue is reserved.
 * Return values:
 * < 0: error. skb freed, reference to sock dropped.
 * 0: continue
 * 1: repeat lookup - reference dropped while waiting for socket memory.
 */
int netlink_attachskb(struct sock *sk, struct sk_buff *skb, int nonblock, long timeo)
{
	struct netlink_opt *nlk;

	nlk = nlk_sk(sk);

#ifdef NL_EMULATE_DEV
	if (nlk->handler)
		return 0;
#endif
	if (atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf ||
	    test_bit(0, &nlk->state)) {
		DECLARE_WAITQUEUE(wait, current);
		if (!timeo) {
			if (!nlk->pid)
				netlink_overrun(sk);
			sock_put(sk);
			kfree_skb(skb);
			return -EAGAIN;
		}

		__set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&nlk->wait, &wait);

		if ((atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf ||
		     test_bit(0, &nlk->state)) &&
		    !sock_flag(sk, SOCK_DEAD))
			timeo = schedule_timeout(timeo);

		__set_current_state(TASK_RUNNING);
		remove_wait_queue(&nlk->wait, &wait);
		sock_put(sk);

		if (signal_pending(current)) {
			kfree_skb(skb);
			return sock_intr_errno(timeo);
		}
		return 1;
	}
        /* 直接将skb的属主设置为接收skb的sock */
	skb_set_owner_r(skb, sk);
	return 0;
}

/* 将skb添加到接收sock的接收队列当中 */
int netlink_sendskb(struct sock *sk, struct sk_buff *skb, int protocol)
{
	struct netlink_opt *nlk;
	int len = skb->len;

	nlk = nlk_sk(sk);
#ifdef NL_EMULATE_DEV
	if (nlk->handler) {
		skb_orphan(skb);
		len = nlk->handler(protocol, skb);
		sock_put(sk);
		return len;
	}
#endif

	skb_queue_tail(&sk->sk_receive_queue, skb);
	sk->sk_data_ready(sk, len);
	sock_put(sk);
	return len;
}

void netlink_detachskb(struct sock *sk, struct sk_buff *skb)
{
	kfree_skb(skb);
	sock_put(sk);
}

static inline struct sk_buff *netlink_trim(struct sk_buff *skb, int allocation)
{
	int delta;

        /* 让skb变成孤儿，也就是断开skb和sock之间的关系 */
	skb_orphan(skb);

        /* 如果数据占用一大半，则直接返回  */
	delta = skb->end - skb->tail;
	if (delta * 2 < skb->truesize)
		return skb;

	if (skb_shared(skb)) {
		struct sk_buff *nskb = skb_clone(skb, allocation);
		if (!nskb)
			return skb;
		kfree_skb(skb);
		skb = nskb;
	}

	if (!pskb_expand_head(skb, 0, -delta, allocation))
		skb->truesize -= delta;

	return skb;
}

/* 想目的进程发送消息，
  * ssk表示发送消息的sock 
  * skb表示发送数据包 
  * pid表示目的进程  
  */
int netlink_unicast(struct sock *ssk, struct sk_buff *skb, u32 pid, int nonblock)
{
	struct sock *sk;
	int err;
	long timeo;

	skb = netlink_trim(skb, gfp_any());

	timeo = sock_sndtimeo(ssk, nonblock);
retry:
	sk = netlink_getsockbypid(ssk, pid);
	if (IS_ERR(sk)) {
		kfree_skb(skb);
		return PTR_ERR(sk);
	}
	err = netlink_attachskb(sk, skb, nonblock, timeo);
	if (err == 1)
		goto retry;
	if (err)
		return err;

	return netlink_sendskb(sk, skb, ssk->sk_protocol);
}

/* 将skb广播到对应的sk当中，
  * 返回0，1表示正常
  */
static __inline__ int netlink_broadcast_deliver(struct sock *sk, struct sk_buff *skb)
{
	struct netlink_opt *nlk = nlk_sk(sk);
#ifdef NL_EMULATE_DEV
	if (nlk->handler) {
		nlk->handler(sk->sk_protocol, skb);
		return 0;
	} else
#endif
	if (atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf &&
	    !test_bit(0, &nlk->state)) {
		skb_set_owner_r(skb, sk);
		skb_queue_tail(&sk->sk_receive_queue, skb);
		sk->sk_data_ready(sk, skb->len);
		return atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf;
	}
	return -1;
}

struct netlink_broadcast_data {
	struct sock *exclude_sk;         /* 排除向exclude_sk发送 */
	u32 pid;                                /* 目的进程号 */
	u32 group;                            /* 目的进程组 */
	int failure;
	int congested;
	int delivered;
	int allocation;                       /* 内存分配策略 */
	struct sk_buff *skb,               /* 需要被发送的skb */
                            *skb2;               /* 表示clone的skb,然后发送到不同的socket */
};

/* sk表示要接收消息的socket，其中有条件判断，是否可以将消息发送给它
  * p表示多播发送的信息 
  */
static inline int do_one_broadcast(struct sock *sk,
				   struct netlink_broadcast_data *p)
{
	struct netlink_opt *nlk = nlk_sk(sk);
	int val;

        /* 如果是自己的话就直接返回 */
	if (p->exclude_sk == sk)
		goto out;

        /* 如果和目的进程的pid相同，则多播就不需要发送，
          * 如果和目的多播组不在一组，则不发送数据包
          */
	if (nlk->pid == p->pid || !(nlk->groups & p->group))
		goto out;

        /*  如果有一个失败，则停止并发送错误报告 */
	if (p->failure) {
		netlink_overrun(sk);
		goto out;
	}

	sock_hold(sk);
	if (p->skb2 == NULL) {
		if (atomic_read(&p->skb->users) != 1) {
			p->skb2 = skb_clone(p->skb, p->allocation);
		} else {
			p->skb2 = p->skb;
			atomic_inc(&p->skb->users);
		}
	}
        /* 如果拷贝的skb失败，则这是错误标记 */
	if (p->skb2 == NULL) {
		netlink_overrun(sk);
		/* Clone failed. Notify ALL listeners. */
		p->failure = 1;
	} else if ((val = netlink_broadcast_deliver(sk, p->skb2)) < 0) {
		netlink_overrun(sk);
	} else {
		p->congested |= val;
		p->delivered = 1;
                /* 在skb2正确广播之后，skb2就要重新设置为null */
		p->skb2 = NULL;
	}
	sock_put(sk);

out:
	return 0;
}

/* ssk表示发送消息的sock
  * skb表示需要发送的skb 
  * pid表示目的进程号 
  * group表示目的组播组 
  * allocation表示内存分配策略 
  */
int netlink_broadcast(struct sock *ssk, struct sk_buff *skb, u32 pid,
		      u32 group, int allocation)
{
	struct netlink_broadcast_data info;
	struct hlist_node *node;
	struct sock *sk;

	skb = netlink_trim(skb, allocation);

        /* 设置排除sock */
	info.exclude_sk = ssk;
	info.pid = pid;
	info.group = group;
	info.failure = 0;
	info.congested = 0;
	info.delivered = 0;
	info.allocation = allocation;
	info.skb = skb;
	info.skb2 = NULL;

	/* While we sleep in clone, do not allow to change socket list */

	netlink_lock_table();

        /* 扫描本地套接字所在的多播组链表，注意第三个参数表示所有
          * 和发送套接字协议相同的加入多播组的socket
          */
	sk_for_each_bound(sk, node, &nl_table[ssk->sk_protocol].mc_list)
		do_one_broadcast(sk, &info);

	netlink_unlock_table();

	if (info.skb2)
		kfree_skb(info.skb2);
	kfree_skb(skb);

	if (info.delivered) {
		if (info.congested && (allocation & __GFP_WAIT))
			yield();
		return 0;
	}
	if (info.failure)
		return -ENOBUFS;
	return -ESRCH;
}

struct netlink_set_err_data {
	struct sock *exclude_sk;
	u32 pid;
	u32 group;
	int code;
};

static inline int do_one_set_err(struct sock *sk,
				 struct netlink_set_err_data *p)
{
	struct netlink_opt *nlk = nlk_sk(sk);

	if (sk == p->exclude_sk)
		goto out;

	if (nlk->pid == p->pid || !(nlk->groups & p->group))
		goto out;

	sk->sk_err = p->code;
	sk->sk_error_report(sk);
out:
	return 0;
}

void netlink_set_err(struct sock *ssk, u32 pid, u32 group, int code)
{
	struct netlink_set_err_data info;
	struct hlist_node *node;
	struct sock *sk;

	info.exclude_sk = ssk;
	info.pid = pid;
	info.group = group;
	info.code = code;

	read_lock(&nl_table_lock);

	sk_for_each_bound(sk, node, &nl_table[ssk->sk_protocol].mc_list)
		do_one_set_err(sk, &info);

	read_unlock(&nl_table_lock);
}

static inline void netlink_rcv_wake(struct sock *sk)
{
	struct netlink_opt *nlk = nlk_sk(sk);

	if (!skb_queue_len(&sk->sk_receive_queue))
		clear_bit(0, &nlk->state);
	if (!test_bit(0, &nlk->state))
		wake_up_interruptible(&nlk->wait);
}

/* netlink的sendto函数的具体实现，
  * len表示发送数据长度 
  * sock表示发送数据套接字 
  * msg表示发送的数据和目的地址 
  */
static int netlink_sendmsg(struct kiocb *kiocb, struct socket *sock,
			   struct msghdr *msg, size_t len)
{
	struct sock_iocb *siocb = kiocb_to_siocb(kiocb);
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk = nlk_sk(sk);
        /* 设置接收消息的地址 */
	struct sockaddr_nl *addr=msg->msg_name;
	u32 dst_pid;
	u32 dst_groups;
	struct sk_buff *skb;
	int err;
	struct scm_cookie scm;

	if (msg->msg_flags&MSG_OOB)
		return -EOPNOTSUPP;

	if (NULL == siocb->scm)
		siocb->scm = &scm;
	err = scm_send(sock, msg, siocb->scm);
	if (err < 0)
		return err;

	if (msg->msg_namelen) {
		if (addr->nl_family != AF_NETLINK)
			return -EINVAL;
                /* 设置目的pid和组播组 */
		dst_pid = addr->nl_pid;
		dst_groups = addr->nl_groups;
		if (dst_groups && !netlink_capable(sock, NL_NONROOT_SEND))
			return -EPERM;
	} else {
		dst_pid = nlk->dst_pid;
		dst_groups = nlk->dst_groups;
	}

        /* 如果发送的进程pid为0，则进行自动绑定  */
	if (!nlk->pid) {
		err = netlink_autobind(sock);
		if (err)
			goto out;
	}

	err = -EMSGSIZE;
	if (len > sk->sk_sndbuf - 32)
		goto out;
	err = -ENOBUFS;
	skb = alloc_skb(len, GFP_KERNEL);
	if (skb==NULL)
		goto out;

        /* 初始化struct netlink_skb_parms */
	NETLINK_CB(skb).pid	= nlk->pid;
	NETLINK_CB(skb).groups	= nlk->groups;
	NETLINK_CB(skb).dst_pid = dst_pid;
	NETLINK_CB(skb).dst_groups = dst_groups;
	memcpy(NETLINK_CREDS(skb), &siocb->scm->creds, sizeof(struct ucred));

	/* What can I do? Netlink is asynchronous, so that
	   we will have to save current capabilities to
	   check them, when this message will be delivered
	   to corresponding kernel module.   --ANK (980802)
	 */

	err = -EFAULT;
        /* 拷贝需要发送的数据，并生成一个skb */
	if (memcpy_fromiovec(skb_put(skb,len), msg->msg_iov, len)) {
		kfree_skb(skb);
		goto out;
	}

        /* 做安全性判断 */
	err = security_netlink_send(sk, skb);
	if (err) {
		kfree_skb(skb);
		goto out;
	}

        /* 如果需要把消息发送到几个组播组 */
	if (dst_groups) {
		atomic_inc(&skb->users);
		netlink_broadcast(sk, skb, dst_pid, dst_groups, GFP_KERNEL);
	}
	err = netlink_unicast(sk, skb, dst_pid, msg->msg_flags&MSG_DONTWAIT);

out:
	return err;
}

/* netlink协议族的消息接收函数 */
static int netlink_recvmsg(struct kiocb *kiocb, struct socket *sock,
			   struct msghdr *msg, size_t len,
			   int flags)
{
	struct sock_iocb *siocb = kiocb_to_siocb(kiocb);
	struct scm_cookie scm;
	struct sock *sk = sock->sk;
	struct netlink_opt *nlk = nlk_sk(sk);
	int noblock = flags&MSG_DONTWAIT;
	size_t copied;
	struct sk_buff *skb;
	int err;

	if (flags&MSG_OOB)
		return -EOPNOTSUPP;

	copied = 0;

	skb = skb_recv_datagram(sk,flags,noblock,&err);
	if (skb==NULL)
		goto out;

	msg->msg_namelen = 0;

	copied = skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	skb->h.raw = skb->data;
	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);

	if (msg->msg_name) {
		struct sockaddr_nl *addr = (struct sockaddr_nl*)msg->msg_name;
		addr->nl_family = AF_NETLINK;
		addr->nl_pad    = 0;
		addr->nl_pid	= NETLINK_CB(skb).pid;
		addr->nl_groups	= NETLINK_CB(skb).dst_groups;
		msg->msg_namelen = sizeof(*addr);
	}

	if (NULL == siocb->scm) {
		memset(&scm, 0, sizeof(scm));
		siocb->scm = &scm;
	}
	siocb->scm->creds = *NETLINK_CREDS(skb);
	skb_free_datagram(sk, skb);

	if (nlk->cb && atomic_read(&sk->sk_rmem_alloc) <= sk->sk_rcvbuf / 2)
		netlink_dump(sk);

	scm_recv(sock, msg, siocb->scm, flags);

out:
	netlink_rcv_wake(sk);
	return err ? : copied;
}

/* 调用数据准备好回调函数，同时唤醒等待进程 */
static void netlink_data_ready(struct sock *sk, int len)
{
	struct netlink_opt *nlk = nlk_sk(sk);

	if (nlk->data_ready)
		nlk->data_ready(sk, len);
	netlink_rcv_wake(sk);
}

/*
 *	We export these functions to other modules. They provide a 
 *	complete set of kernel non-blocking support for message
 *	queueing.
 */
/* 内核空间创建netlink套接字函数，input表示内核套接字的接收回调函数
  * unit表示协议 
  */
struct sock *
netlink_kernel_create(int unit, void (*input)(struct sock *sk, int len))
{
	struct socket *sock;
	struct sock *sk;

	if (!nl_table)
		return NULL;

	if (unit<0 || unit>=MAX_LINKS)
		return NULL;

        /* 内核创建套接字 */
	if (sock_create_lite(PF_NETLINK, SOCK_DGRAM, unit, &sock))
		return NULL;

	if (netlink_create(sock, unit) < 0) {
		sock_release(sock);
		return NULL;
	}
	sk = sock->sk;
	sk->sk_data_ready = netlink_data_ready;
	if (input)
		nlk_sk(sk)->data_ready = input;

        /* 注意此时创建创建的套接字的pid为0  */
	if (netlink_insert(sk, 0)) {
		sock_release(sock);
		return NULL;
	}
	return sk;
}

void netlink_set_nonroot(int protocol, unsigned int flags)
{ 
	if ((unsigned int)protocol < MAX_LINKS) 
		nl_table[protocol].nl_nonroot = flags;
} 

static void netlink_destroy_callback(struct netlink_callback *cb)
{
	if (cb->skb)
		kfree_skb(cb->skb);
	kfree(cb);
}

/*
 * It looks a bit ugly.
 * It would be better to create kernel thread.
 */

static int netlink_dump(struct sock *sk)
{
	struct netlink_opt *nlk = nlk_sk(sk);
	struct netlink_callback *cb;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int len;
	
	skb = sock_rmalloc(sk, NLMSG_GOODSIZE, 0, GFP_KERNEL);
	if (!skb)
		return -ENOBUFS;

	spin_lock(&nlk->cb_lock);

	cb = nlk->cb;
	if (cb == NULL) {
		spin_unlock(&nlk->cb_lock);
		kfree_skb(skb);
		return -EINVAL;
	}

	len = cb->dump(skb, cb);

	if (len > 0) {
		spin_unlock(&nlk->cb_lock);
		skb_queue_tail(&sk->sk_receive_queue, skb);
		sk->sk_data_ready(sk, len);
		return 0;
	}

	nlh = __nlmsg_put(skb, NETLINK_CB(cb->skb).pid, cb->nlh->nlmsg_seq, NLMSG_DONE, sizeof(int));
	nlh->nlmsg_flags |= NLM_F_MULTI;
	memcpy(NLMSG_DATA(nlh), &len, sizeof(len));
	skb_queue_tail(&sk->sk_receive_queue, skb);
	sk->sk_data_ready(sk, skb->len);

	cb->done(cb);
	nlk->cb = NULL;
	spin_unlock(&nlk->cb_lock);

	netlink_destroy_callback(cb);
	sock_put(sk);
	return 0;
}

int netlink_dump_start(struct sock *ssk, struct sk_buff *skb,
		       struct nlmsghdr *nlh,
		       int (*dump)(struct sk_buff *skb, struct netlink_callback*),
		       int (*done)(struct netlink_callback*))
{
	struct netlink_callback *cb;
	struct sock *sk;
	struct netlink_opt *nlk;

	cb = kmalloc(sizeof(*cb), GFP_KERNEL);
	if (cb == NULL)
		return -ENOBUFS;

	memset(cb, 0, sizeof(*cb));
	cb->dump = dump;
	cb->done = done;
	cb->nlh = nlh;
	atomic_inc(&skb->users);
	cb->skb = skb;

	sk = netlink_lookup(ssk->sk_protocol, NETLINK_CB(skb).pid);
	if (sk == NULL) {
		netlink_destroy_callback(cb);
		return -ECONNREFUSED;
	}
	nlk = nlk_sk(sk);
	/* A dump is in progress... */
	spin_lock(&nlk->cb_lock);
	if (nlk->cb) {
		spin_unlock(&nlk->cb_lock);
		netlink_destroy_callback(cb);
		sock_put(sk);
		return -EBUSY;
	}
	nlk->cb = cb;
	spin_unlock(&nlk->cb_lock);

	netlink_dump(sk);
	return 0;
}

void netlink_ack(struct sk_buff *in_skb, struct nlmsghdr *nlh, int err)
{
	struct sk_buff *skb;
	struct nlmsghdr *rep;
	struct nlmsgerr *errmsg;
	int size;

	if (err == 0)
		size = NLMSG_SPACE(sizeof(struct nlmsgerr));
	else
		size = NLMSG_SPACE(4 + NLMSG_ALIGN(nlh->nlmsg_len));

	skb = alloc_skb(size, GFP_KERNEL);
	if (!skb) {
		struct sock *sk;

		sk = netlink_lookup(in_skb->sk->sk_protocol,
				    NETLINK_CB(in_skb).pid);
		if (sk) {
			sk->sk_err = ENOBUFS;
			sk->sk_error_report(sk);
			sock_put(sk);
		}
		return;
	}

	rep = __nlmsg_put(skb, NETLINK_CB(in_skb).pid, nlh->nlmsg_seq,
			  NLMSG_ERROR, sizeof(struct nlmsgerr));
	errmsg = NLMSG_DATA(rep);
	errmsg->error = err;
	memcpy(&errmsg->msg, nlh, err ? nlh->nlmsg_len : sizeof(struct nlmsghdr));
	netlink_unicast(in_skb->sk, skb, NETLINK_CB(in_skb).pid, MSG_DONTWAIT);
}


#ifdef CONFIG_PROC_FS
struct nl_seq_iter {
	int link;
	int hash_idx;
};

static struct sock *netlink_seq_socket_idx(struct seq_file *seq, loff_t pos)
{
	struct nl_seq_iter *iter = seq->private;
	int i, j;
	struct sock *s;
	struct hlist_node *node;
	loff_t off = 0;

	for (i=0; i<MAX_LINKS; i++) {
		struct nl_pid_hash *hash = &nl_table[i].hash;

		for (j = 0; j <= hash->mask; j++) {
			sk_for_each(s, node, &hash->table[j]) {
				if (off == pos) {
					iter->link = i;
					iter->hash_idx = j;
					return s;
				}
				++off;
			}
		}
	}
	return NULL;
}

static void *netlink_seq_start(struct seq_file *seq, loff_t *pos)
{
	read_lock(&nl_table_lock);
	return *pos ? netlink_seq_socket_idx(seq, *pos - 1) : SEQ_START_TOKEN;
}

static void *netlink_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct sock *s;
	struct nl_seq_iter *iter;
	int i, j;

	++*pos;

	if (v == SEQ_START_TOKEN)
		return netlink_seq_socket_idx(seq, 0);
		
	s = sk_next(v);
	if (s)
		return s;

	iter = seq->private;
	i = iter->link;
	j = iter->hash_idx + 1;

	do {
		struct nl_pid_hash *hash = &nl_table[i].hash;

		for (; j <= hash->mask; j++) {
			s = sk_head(&hash->table[j]);
			if (s) {
				iter->link = i;
				iter->hash_idx = j;
				return s;
			}
		}

		j = 0;
	} while (++i < MAX_LINKS);

	return NULL;
}

static void netlink_seq_stop(struct seq_file *seq, void *v)
{
	read_unlock(&nl_table_lock);
}


static int netlink_seq_show(struct seq_file *seq, void *v)
{
	if (v == SEQ_START_TOKEN)
		seq_puts(seq,
			 "sk       Eth Pid    Groups   "
			 "Rmem     Wmem     Dump     Locks\n");
	else {
		struct sock *s = v;
		struct netlink_opt *nlk = nlk_sk(s);

		seq_printf(seq, "%p %-3d %-6d %08x %-8d %-8d %p %d\n",
			   s,
			   s->sk_protocol,
			   nlk->pid,
			   nlk->groups,
			   atomic_read(&s->sk_rmem_alloc),
			   atomic_read(&s->sk_wmem_alloc),
			   nlk->cb,
			   atomic_read(&s->sk_refcnt)
			);

	}
	return 0;
}

static struct seq_operations netlink_seq_ops = {
	.start  = netlink_seq_start,
	.next   = netlink_seq_next,
	.stop   = netlink_seq_stop,
	.show   = netlink_seq_show,
};


static int netlink_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	struct nl_seq_iter *iter;
	int err;

	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return -ENOMEM;

	err = seq_open(file, &netlink_seq_ops);
	if (err) {
		kfree(iter);
		return err;
	}

	memset(iter, 0, sizeof(*iter));
	seq = file->private_data;
	seq->private = iter;
	return 0;
}

static struct file_operations netlink_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= netlink_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

#endif

/* 注册到netlink通知链 */
int netlink_register_notifier(struct notifier_block *nb)
{
	return notifier_chain_register(&netlink_chain, nb);
}

int netlink_unregister_notifier(struct notifier_block *nb)
{
	return notifier_chain_unregister(&netlink_chain, nb);
}
                
static struct proto_ops netlink_ops = {
	.family =	PF_NETLINK,
	.owner =	THIS_MODULE,
	.release =	netlink_release,
	.bind =		netlink_bind,
	.connect =	netlink_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	sock_no_accept,
	.getname =	netlink_getname,
	.poll =		datagram_poll,
	.ioctl =	sock_no_ioctl,
	.listen =	sock_no_listen,
	.shutdown =	sock_no_shutdown,
	.setsockopt =	sock_no_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =	netlink_sendmsg,
	.recvmsg =	netlink_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
};

static struct net_proto_family netlink_family_ops = {
	.family = PF_NETLINK,
	.create = netlink_create,
	.owner	= THIS_MODULE,	/* for consistency 8) */
};

extern void netlink_skb_parms_too_large(void);

/* netlink协议的初始化 */
static int __init netlink_proto_init(void)
{
	struct sk_buff *dummy_skb;
	int i;
	unsigned long max;
	unsigned int order;

	if (sizeof(struct netlink_skb_parms) > sizeof(dummy_skb->cb))
		netlink_skb_parms_too_large();

        /* 分配足够数量的结构体 */
	nl_table = kmalloc(sizeof(*nl_table) * MAX_LINKS, GFP_KERNEL);
	if (!nl_table) {
enomem:
		printk(KERN_CRIT "netlink_init: Cannot allocate nl_table\n");
		return -ENOMEM;
	}

	memset(nl_table, 0, sizeof(*nl_table) * MAX_LINKS);

	if (num_physpages >= (128 * 1024))
		max = num_physpages >> (21 - PAGE_SHIFT);
	else
		max = num_physpages >> (23 - PAGE_SHIFT);

	order = get_bitmask_order(max) - 1 + PAGE_SHIFT;
	max = (1UL << order) / sizeof(struct hlist_head);
	order = get_bitmask_order(max > UINT_MAX ? UINT_MAX : max) - 1;

	for (i = 0; i < MAX_LINKS; i++) {
		struct nl_pid_hash *hash = &nl_table[i].hash;

		hash->table = nl_pid_hash_alloc(1 * sizeof(*hash->table));
		if (!hash->table) {
			while (i-- > 0)
				nl_pid_hash_free(nl_table[i].hash.table,
						 1 * sizeof(*hash->table));
			kfree(nl_table);
			goto enomem;
		}
		memset(hash->table, 0, 1 * sizeof(*hash->table));
		hash->max_shift = order;
		hash->shift = 0;
		hash->mask = 0;
		hash->rehash_time = jiffies;
	}

	sock_register(&netlink_family_ops);
#ifdef CONFIG_PROC_FS
	proc_net_fops_create("netlink", 0, &netlink_seq_fops);
#endif
	/* The netlink device handler may be needed early. */ 
        /* 路由套接字初始化 */
	rtnetlink_init();
	return 0;
}

static void __exit netlink_proto_exit(void)
{
       sock_unregister(PF_NETLINK);
       proc_net_remove("netlink");
       kfree(nl_table);
       nl_table = NULL;
}

core_initcall(netlink_proto_init);
module_exit(netlink_proto_exit);

MODULE_LICENSE("GPL");

MODULE_ALIAS_NETPROTO(PF_NETLINK);

EXPORT_SYMBOL(netlink_ack);
EXPORT_SYMBOL(netlink_broadcast);
EXPORT_SYMBOL(netlink_dump_start);
EXPORT_SYMBOL(netlink_kernel_create);
EXPORT_SYMBOL(netlink_register_notifier);
EXPORT_SYMBOL(netlink_set_err);
EXPORT_SYMBOL(netlink_set_nonroot);
EXPORT_SYMBOL(netlink_unicast);
EXPORT_SYMBOL(netlink_unregister_notifier);

