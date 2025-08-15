// // #include "types.h"
// // #include "stat.h"
// // #include "user.h"
// // #define NUM_PROCS 5 // Number of processes to create

// // // int main()
// // {
// //     for (int i = 0; i < NUM_PROCS; i++)
// //     {
// //         int pid = custom_fork(1, 30); // Start later, execution time 50
// //         if (pid < 0)
// //         {
// //             printf(1, "Failed to fork process %d\n", i);
// //             exit();
// //         }
// //         else if (pid == 0)
// //         {
// //             // Child process
// //             printf(1, "Child %d (PID: %d) started but should not run yet.\n", i, getpid());
// //             for (volatile int j = 0; j < 100000000; j++)
// //                 ; // Simulated work
// //             exit();
// //         }
// //     }
// //     printf(1, "All child processes created with start_later flag set.\n");
// //     sleep(400);
// //     printf(1, "Calling sys_scheduler_start() to allow execution.\n");
// //     scheduler_start();
// //     for (int i = 0; i < NUM_PROCS; i++)
// //     {
// //         wait();
// //     }
// //     printf(1, "All child processes completed.\n");
// //     exit();
// // }



// #include "types.h"
// #include "stat.h"
// #include "user.h"

// int fib(int n) {
//     if (n <= 0) return 0;
//     if (n == 1) return 1;
//     if (n == 2) return 1;
//     return fib(n - 1) + fib(n - 2);
// }

// int main() {
//     int pid = fork();
//     if (pid != 0) {
//         while (1) {
//             printf(1, "Hello, I am parent\n");
//             fib(40);
//         }
//     } else {
//         while (1) {
//             printf(1, "Hi there, I am child\n");
//             fib(40);
//         }
//     }
// }
# include "types.h"
# include "stat.h"
# include "user.h"
int fib(int n) {
if (n <= 0) return 0;
if (n == 1) return 1;
if (n == 2) return 1;
return fib (n - 1) + fib(n - 2) ;
}
void sv () {
printf (1 , "I am Shivam \n") ;
}
void myHandler () {
printf (1 , "I am inside the handler \n") ;
sv () ;
}
int main () {
signal ( myHandler ) ; // you need to implement this syscall for registering signal handlers
while (1) {
printf (1 , " This is normal code running \n") ;
fib (35) ; // doing CPU intensive work
}
}