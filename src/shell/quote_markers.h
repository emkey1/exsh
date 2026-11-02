#ifndef SHELL_QUOTE_MARKERS_H
#define SHELL_QUOTE_MARKERS_H

#define SHELL_QUOTE_MARK_SINGLE ((char)0x01)
#define SHELL_QUOTE_MARK_DOUBLE ((char)0x02)
/* The next byte is itself, for a $'\\001' that would otherwise read as a marker. */
#define SHELL_QUOTE_MARK_BYTE ((char)0x03)

#endif /* SHELL_QUOTE_MARKERS_H */
