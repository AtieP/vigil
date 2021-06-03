/*  
    This file is part of Vigil.

    Vigil is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Vigil is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Vigil.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cpu/apic/apic.h>
#include <cpu/gdt.h>
#include <cpu/idt.h>
#include <cpu/pio.h>
#include <misc/kcon.h>
#include <mm/mm.h>
#include <proc/sched.h>
#include <tools/builtins.h>
#include <tools/vector.h>

#define MODULE_NAME "sched"

static struct vector processes;
static pid_t current_pid = 0;
static tid_t current_tid = 0;

__attribute__((__noreturn__)) static void sched_next(struct interrupt_frame *gprs) {
    struct sched_process *current_process = vector_get(&processes, current_pid);
    struct sched_thread *current_thread = vector_get(&current_process->threads, current_tid);
    struct sched_thread *next_thread;
    if (gprs) {
        // save general purpose register state
        memcpy(&current_thread->gprs, gprs, sizeof(struct interrupt_frame));
    }
    // get next thread to execute
    if (current_tid < current_process->threads.items - 1) {
        current_tid++;
        next_thread = vector_get(&current_process->threads, current_tid);
    } else if (current_pid < processes.items - 1) {
        current_pid++;
        current_tid = 0;
        next_thread = vector_get(&((struct sched_process *) vector_get(&processes, current_pid))->threads, current_tid);
    } else {
        current_pid = 0;
        current_tid = 0;
        next_thread = vector_get(&((struct sched_process *) vector_get(&processes, current_pid))->threads, current_tid);
    }
    lapic_eoi();
    asm volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rax\n\t"
        "add $16, %%rsp\n\t" // reserved1 and reserved2
        "iretq\n\t"
        :: "r"(&next_thread->gprs)
        : "memory"
    );
    __builtin_unreachable();
}

__attribute__((__noreturn__)) void sched_init(uintptr_t address) {
    struct sched_process kernel;
    vector_create(&kernel.threads, sizeof(struct sched_thread));
    vector_create(&processes, sizeof(struct sched_process));
    vector_push(&processes, &kernel);
    sched_new_kernel_thread(address);
    ioapic_redirect_irq(lapic_get_id(), 0, idt_allocate_interrupt((idt_handler_t) sched_next), 0);
    sched_next(NULL);
    __builtin_unreachable();
}

tid_t sched_new_kernel_thread(uintptr_t address) {
    struct sched_process *kernel = vector_get(&processes, 0);
    struct sched_thread thread;
    thread.tid = kernel->threads.items - 1;
    thread.gprs.cs = GDT_KERNEL_CODE64_SEL;
    thread.gprs.ss = GDT_KERNEL_DATA_SEL;
    thread.gprs.rip = address;
    thread.gprs.rflags = 0x202;
    thread.gprs.rsp = (uintptr_t) pmm_calloc(MM_PAGE_SIZE) + MM_HIGHER_BASE;
    vector_push(&kernel->threads, &thread);
    return thread.tid;
}

void sched_kill_kernel_thread(tid_t tid) {
    struct sched_process *kernel = vector_get(&processes, 0);
    struct sched_thread *thread = vector_get(&kernel->threads, tid);
    if (!thread) {
        kcon_log(KCON_LOG_WARN, MODULE_NAME, "Could not get kernel thread with TID %d", tid);
    }
    kheap_free(thread);
    vector_remove(&kernel->threads, tid);
}
