#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#define SCHED_MYCFS	7

const char *sched_policy_to_string(unsigned int policy)
{
	switch(policy) {
	case SCHED_OTHER:
		return "SCHED_OTHER";
	/* first in first out */
	case SCHED_FIFO:
		return "SCHED_FIFO";
	/* round-robin */
	case SCHED_RR:
		return "SCHED_RR";
	case SCHED_BATCH:
		return "SCHED_BATCH";
	case SCHED_IDLE:
		return "SCHED_IDLE";
	case SCHED_MYCFS:
		return "SCHED_MYCFS";
	
	default:
		return "Error class";
	}
}

unsigned long long counter = 0;

void sigalrm_sighand(void) {
	printf("PID:%d, timer went off, counter: %llu, counter reset\n",
		getpid(), counter);
	counter = 0;
}

int main()
{
	struct sched_param param = { 0 };
	int ret, policy;
	pid_t pid;
	struct itimerval it;

	ret = sched_setscheduler(0, SCHED_MYCFS, &param);
	if (ret < 0) {
		perror("sched_setscheduler: ");
	}

	if (pid = fork() == -1) {
		perror("Fork: ");
		exit(EXIT_FAILURE);
	}

	/* code after fork */
	policy = sched_getscheduler(0);
	printf("PID: %d, get scheduler policy: %s\n",
		getpid(), sched_policy_to_string(policy));

	if (signal(SIGALRM, (sighandler_t) sigalrm_sighand) == SIG_ERR) {
		perror("Unable to catch SIGALRM");
		exit(1);
	}

	it.it_value.tv_sec = 5;
	it.it_value.tv_usec = 0;
	it.it_interval = it.it_value;

	if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
		perror("setitimer: ");
		exit(1);
	}

	printf("PID:%d setitimer as 5s.\n", getpid());

	while(1) 
		counter++;

	return 0;

}
