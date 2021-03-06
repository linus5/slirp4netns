#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include <libslirp.h>

static int nsenter(pid_t target_pid)
{
	char userns[32], netns[32];
	int usernsfd, netnsfd;
	snprintf(userns, sizeof(userns), "/proc/%d/ns/user", target_pid);
	snprintf(netns, sizeof(netns), "/proc/%d/ns/net", target_pid);
	if ((usernsfd = open(userns, O_RDONLY)) < 0) {
		perror(userns);
		return usernsfd;
	}
	if ((netnsfd = open(netns, O_RDONLY)) < 0) {
		perror(netns);
		return netnsfd;
	}
	if (setns(usernsfd, CLONE_NEWUSER) < 0) {
		perror("setns(CLONE_NEWUSER)");
		return -1;
	}
	if (setns(netnsfd, CLONE_NEWNET) < 0) {
		perror("setns(CLONE_NEWNET)");
		return -1;
	}
	return 0;
}

static int open_tap(const char *tapname)
{
	int fd;
	struct ifreq ifr;
	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		perror("open(\"/dev/net/tun\")");
		return fd;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, tapname, sizeof(ifr.ifr_name) - 1);
	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
		perror("ioctl(TUNSETIFF)");
		close(fd);
		return -1;
	}
	return fd;
}

static int sendfd(int sock, int fd)
{
	ssize_t rc;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	char cmsgbuf[CMSG_SPACE(sizeof(fd))];
	struct iovec iov;
	char dummy = '\0';
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = &dummy;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	msg.msg_controllen = cmsg->cmsg_len;
	if ((rc = sendmsg(sock, &msg, 0)) < 0) {
		perror("sendmsg");
	}
	return rc;
}

static int child(int sock, pid_t target_pid, const char *tapname)
{
	int rc, tapfd;
	if ((rc = nsenter(target_pid)) < 0) {
		return rc;
	}
	if ((tapfd = open_tap(tapname)) < 0) {
		return tapfd;
	}
	if (sendfd(sock, tapfd) < 0) {
		close(tapfd);
		close(sock);
		return -1;
	}
	fprintf(stderr, "sent tapfd=%d for %s\n", tapfd, tapname);
	close(sock);
	return 0;
}

static int recvfd(int sock)
{
	int fd;
	ssize_t rc;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	char cmsgbuf[CMSG_SPACE(sizeof(fd))];
	struct iovec iov;
	char dummy = '\0';
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = &dummy;
	iov.iov_len = 1;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	if ((rc = recvmsg(sock, &msg, 0)) < 0) {
		perror("recvmsg");
		return (int)rc;
	}
	if (rc == 0) {
		fprintf(stderr, "the message is empty\n");
		return -1;
	}
	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS) {
		fprintf(stderr, "the message does not contain fd\n");
		return -1;
	}
	memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
	return fd;
}

struct slirp2tap_data {
	int tapfd, slirpfd;
};

#define ETH_BUF_SIZE (65536)

// TODO: look into whether we can use splice(2). probably we cannot.
static void *slirp2tap_thread(void *p)
{
	ssize_t rc;
	struct slirp2tap_data *data = (struct slirp2tap_data *)p;
	unsigned char *buf = (unsigned char *)malloc(ETH_BUF_SIZE);
	while (1) {
		if ((rc = read(data->slirpfd, buf, ETH_BUF_SIZE)) < 0) {
			perror("slirp2tap_thread: read");
			free(buf);
			exit(EXIT_FAILURE);
		}
		if ((rc = write(data->tapfd, buf, rc)) < 0) {
			perror("slirp2tap_thread: write");
			free(buf);
			exit(EXIT_FAILURE);
		}
	}
	/* NOTREACHED */
	free(buf);
}

static int tap2slirp(SLIRP *slirp, int tapfd)
{
	ssize_t rc;
	unsigned char *buf = (unsigned char *)malloc(ETH_BUF_SIZE);
	while (1) {
		if ((rc = read(tapfd, buf, ETH_BUF_SIZE)) < 0) {
			perror("tap2slirp: recv");
			free(buf);
			return (int)rc;
		}
		if ((rc = slirp_send(slirp, buf, rc)) < 0) {
			perror("tap2slirp: slirp_send");
			free(buf);
			return (int)rc;
		}
	}
	/* NOTREACHED */
	free(buf);
	return -1;
}

static int do_slirp(int tapfd)
{
	int rc;
	pthread_t thr;
	struct slirp2tap_data thr_data;
	int slirpfd;
	SLIRP *slirp = slirp_open(SLIRP_IPV4);
	if (slirp == NULL) {
		perror("slirp_open");
		return -1;
	}
	if (slirp_start(slirp) < 0) {
		perror("slirp_start");
		slirp_close(slirp);
		return -1;
	}
	// slirp fd can't be used for sending packets to the slirp
	if ((slirpfd = slirp_fd(slirp)) < 0) {
		perror("slirp_fd");
		slirp_close(slirp);
		return slirpfd;
	}
	thr_data.tapfd = tapfd;
	thr_data.slirpfd = slirpfd;
	if ((rc = pthread_create(&thr, NULL, slirp2tap_thread, &thr_data)) != 0) {
		errno = rc;
		perror("pthread_create");
		return -1;
	}
	fprintf(stderr, "READY\n");
	if (tap2slirp(slirp, tapfd) < 0) {
		return -1;
	}
	/* NOTREACHED */
	if ((rc = pthread_join(thr, NULL)) != 0) {
		errno = rc;
		perror("pthread_join");
		return -1;
	}
	return 0;
}

static int parent(int sock)
{
	int rc, tapfd;
	if ((tapfd = recvfd(sock)) < 0) {
		return tapfd;
	}
	fprintf(stderr, "received tapfd=%d\n", tapfd);
	close(sock);
	if ((rc = do_slirp(tapfd)) < 0) {
		close(tapfd);
		return rc;
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s PID TAPNAME\n", argv0);
}

// caller needs to free tapname
static void parse_args(int argc, const char *argv[], pid_t *ptarget_pid, char **tapname)
{
	int target_pid;
	if (argc != 3) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	errno = 0;
	target_pid = strtol(argv[1], NULL, 10);
	if (errno != 0) {
		perror("strtol");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	*ptarget_pid = (pid_t)target_pid;
	*tapname = strdup(argv[2]);
}

int main(int argc, const char *argv[])
{
	int sv[2];
	pid_t target_pid, child_pid;
	char *tapname;
	parse_args(argc, argv, &target_pid, &tapname);
	if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(EXIT_FAILURE);
	}
	if ((child_pid = fork()) < 0) {
		perror("fork");
		free(tapname);
		exit(EXIT_FAILURE);
	}
	if (child_pid == 0) {
		if (child(sv[1], target_pid, tapname) < 0) {
			exit(EXIT_FAILURE);
		}
	} else {
		int child_wstatus, child_status;
		free(tapname);
		tapname = NULL;
		waitpid(child_pid, &child_wstatus, 0);
		if (!WIFEXITED(child_wstatus)) {
			fprintf(stderr, "child failed\n");
			exit(EXIT_FAILURE);
		}
		child_status = WEXITSTATUS(child_wstatus);
		if (child_status != 0) {
			fprintf(stderr, "child failed(%d)\n", child_status);
			exit(child_status);
		}
		if (parent(sv[0]) < 0) {
			fprintf(stderr, "parent failed\n");
			exit(EXIT_FAILURE);
		}
	}
	return 0;
}
