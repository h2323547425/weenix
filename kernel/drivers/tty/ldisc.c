#include "drivers/tty/ldisc.h"
#include <drivers/keyboard.h>
#include <drivers/tty/tty.h>
#include <errno.h>
#include <util/bits.h>
#include <util/debug.h>
#include <util/string.h>

#define ldisc_to_tty(ldisc) CONTAINER_OF((ldisc), tty_t, tty_ldisc)

/**
 * Initialize the line discipline. Don't forget to wipe the buffer associated
 * with the line discipline clean.
 *
 * @param ldisc line discipline.
 */
void ldisc_init(ldisc_t *ldisc)
{
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_init");
    ldisc->ldisc_cooked = 0;
    ldisc->ldisc_tail = 0;
    ldisc->ldisc_head = 0;
    ldisc->ldisc_full = 0;
    sched_queue_init(&ldisc->ldisc_read_queue);
    memset(ldisc->ldisc_buffer, 0, LDISC_BUFFER_SIZE);
}

/**
 * While there are no new characters to be read from the line discipline's
 * buffer, you should make the current thread to sleep on the line discipline's
 * read queue. Note that this sleep can be cancelled. What conditions must be met 
 * for there to be no characters to be read?
 *
 * @param  ldisc the line discipline
 * @param  lock  the lock associated with `ldisc`
 * @return       0 if there are new characters to be read or the ldisc is full.
 *               If the sleep was interrupted, return what
 *               `sched_cancellable_sleep_on` returned (i.e. -EINTR)
 */
long ldisc_wait_read(ldisc_t *ldisc, spinlock_t *lock)
{
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_wait_read");
    while (!ldisc->ldisc_full && ldisc->ldisc_cooked == ldisc->ldisc_tail) {
        int ret = sched_cancellable_sleep_on(&ldisc->ldisc_read_queue, lock);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

/**
 * Reads `count` bytes (at max) from the line discipline's buffer into the
 * provided buffer. Keep in mind the the ldisc's buffer is circular.
 *
 * If you encounter a new line symbol before you have read `count` bytes, you
 * should stop copying and return the bytes read until now.
 * 
 * If you encounter an `EOT` you should stop reading and you should NOT include 
 * the `EOT` in the count of the number of bytes read
 *
 * @param  ldisc the line discipline
 * @param  buf   the buffer to read into.
 * @param  count the maximum number of bytes to read from ldisc.
 * @return       the number of bytes read from the ldisc.
 */
size_t ldisc_read(ldisc_t *ldisc, char *buf, size_t count)
{
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_read");
    int read_count = 0;
    for (size_t i = 0; i < count; i++) {
        // break if no cooked char
        if (ldisc->ldisc_tail == ldisc->ldisc_cooked && !ldisc->ldisc_full) {
            break;
        }
        // get the cooked char and increment tail
        char c = ldisc->ldisc_buffer[ldisc->ldisc_tail];
        ldisc->ldisc_tail = (ldisc->ldisc_tail + 1) % LDISC_BUFFER_SIZE;
        // if char is EOT, stop copying and break
        if (c == EOT) {
            break;
        }
        // if char is \n, stop copying, and break
        if (c == '\n') {
            break;
        }
        // otherwise, increment read_count and copy char
        read_count++;
        buf[i] = c;
    }
    ldisc->ldisc_full = 0;
    return read_count;
}

/**
 * Place the character received into the ldisc's buffer. You should also update
 * relevant fields of the struct.
 *
 * An easier way of handling new characters is making sure that you always have
 * one byte left in the line discipline. This way, if the new character you
 * received is a new line symbol (user hit enter), you can still place the new
 * line symbol into the buffer; if the new character is not a new line symbol,
 * you shouldn't place it into the buffer so that you can leave the space for
 * a new line symbol in the future. 
 * 
 * If the line discipline is full, unless the incoming character is a BS or 
 * ETX, it should not be handled and discarded. 
 *
 * Here are some special cases to consider:
 *      1. If the character is a backspace:
 *          * if there is a character to remove you must also emit a `\b` to
 *            the vterminal.
 *      2. If the character is end of transmission (EOT) character (typing ctrl-d)
 *      3. If the character is end of text (ETX) character (typing ctrl-c)
 *      4. If your buffer is almost full and what you received is not a new line
 *      symbol
 *
 * If you did receive a new line symbol, you should wake up the thread that is
 * sleeping on the wait queue of the line discipline. You should also
 * emit a `\n` to the vterminal by using `vterminal_write`.  
 * 
 * If you encounter the `EOT` character, you should add it to the buffer, 
 * cook the buffer, and wake up the reader (but do not emit an `\n` character 
 * to the vterminal)
 * 
 * In case of `ETX` you should cause the input line to be effectively transformed
 * into a cooked blank line. You should clear uncooked portion of the line, by 
 * adjusting ldisc_head. 
 *
 * Finally, if the none of the above cases apply you should fallback to
 * `vterminal_key_pressed`.
 *
 * Don't forget to write the corresponding characters to the virtual terminal
 * when it applies!
 *
 * @param ldisc the line discipline
 * @param c     the new character
 */
void ldisc_key_pressed(ldisc_t *ldisc, char c)
{
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_key_pressed");
    tty_t *tty = ldisc_to_tty(ldisc);
    if (c == ETX) {
        // delete raw
        ldisc->ldisc_head = ldisc->ldisc_cooked;
        // zero out cooked
        size_t i = ldisc->ldisc_tail;
        while (i != ldisc->ldisc_cooked) {
            ldisc->ldisc_buffer[i] = 0;
            i = (i + 1) % LDISC_BUFFER_SIZE;
        }
        // emit \n to terminal
        vterminal_write(&tty->tty_vterminal, "\n", 1);
        return;
    }
    if (c == EOT) {
        if (!ldisc->ldisc_full) {
            // write EOT and increment head
            ldisc->ldisc_buffer[ldisc->ldisc_head] = EOT;
            ldisc->ldisc_head = (ldisc->ldisc_head + 1) % LDISC_BUFFER_SIZE;
            // check full
            if (ldisc->ldisc_head == ldisc->ldisc_tail) {
                ldisc->ldisc_full = 1;
            }
            // cook all
            ldisc->ldisc_cooked = ldisc->ldisc_head;
            // wake up reading thread
            sched_wakeup_on(&ldisc->ldisc_read_queue, NULL);
        }
        return;
    }
    if (c == '\n') {
        if (!ldisc->ldisc_full) {
            // write \n and increment head
            ldisc->ldisc_buffer[ldisc->ldisc_head] = '\n';
            ldisc->ldisc_head = (ldisc->ldisc_head + 1) % LDISC_BUFFER_SIZE;
            // check full
            if (ldisc->ldisc_head == ldisc->ldisc_tail) {
                ldisc->ldisc_full = 1;
            }
            // cook all
            ldisc->ldisc_cooked = ldisc->ldisc_head;
            // emit \n to terminal
            vterminal_write(&tty->tty_vterminal, "\n", 1);
            // wake up reading thread
            sched_wakeup_on(&ldisc->ldisc_read_queue, NULL);
        }
        return;
    }
    if (c == '\b') {
        if (ldisc->ldisc_cooked != ldisc->ldisc_head) {
            // delete 1 char by decrementing head
            ldisc->ldisc_head = (ldisc->ldisc_head - 1) % LDISC_BUFFER_SIZE;
            // emit \b to terminal
            vterminal_write(&tty->tty_vterminal, "\b", 1);
        }
        return;
    }
    // do nothing when almost full with regular char
    if (ldisc->ldisc_full || (ldisc->ldisc_head + 1) % LDISC_BUFFER_SIZE == ldisc->ldisc_tail) {
        return;
    }
    // otherwise write char and increment head
    ldisc->ldisc_buffer[ldisc->ldisc_head] = c;
    ldisc->ldisc_head = (ldisc->ldisc_head + 1) % LDISC_BUFFER_SIZE;
    vterminal_key_pressed(&tty->tty_vterminal);
}

/**
 * Copy the raw part of the line discipline buffer into the buffer provided.
 *
 * @param  ldisc the line discipline
 * @param  s     the character buffer to write to
 * @return       the number of bytes copied
 */
size_t ldisc_get_current_line_raw(ldisc_t *ldisc, char *s)
{
    // NOT_YET_IMPLEMENTED("DRIVERS: ldisc_get_current_line_raw");
    int read_count = 0;
    size_t i = ldisc->ldisc_cooked;
    while(i != ldisc->ldisc_head) {
        s[read_count++] = ldisc->ldisc_buffer[i];
        i = (i + 1) % LDISC_BUFFER_SIZE;
    }
    return read_count;
}
