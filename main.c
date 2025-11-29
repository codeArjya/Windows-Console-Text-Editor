#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <errno.h>
#include <wincon.h>
#include <time.h>
#include <stdbool.h>

// func declarations
void refreshScreen();
int readKey();
void setStatusMessage(const char* fmt, ...);

void die(const char *s, ...);

// macros
#define CTRL_KEY(k) (k & 0x1f)
#define SB_INIT &(struct StringBuilder){.chars = NULL, .len = 0}
#define TAB_LENGTH 4
#define QUIT_CONFIRMATION 2

enum SpecialKeys
{
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN,
	DELETE_KEY,
	ENTER_KEY,
	BACKSPACE,
	ESCAPE_KEY
};

int clamp(int *val, int min, int max)
{
	*val = *val < min ? min : (*val > max ? max : *val);
	return *val;
}

void resetScreen()
{
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	WriteConsoleA(stdOut, "\x1b[2J\x1b[H", 7, NULL, NULL);
}

/*** buffer operations ***/
struct StringBuilder
{
	char *chars;
	int len;
} *sb = SB_INIT;

void appendToBuffer(const char *add, int len)
{
	char *s = realloc(sb->chars, sb->len + len); // all the data from our string builder is now at s

	if (s == NULL)
		return;
	memcpy(&s[sb->len], add, len);
	sb->chars = s;
	sb->len += len;
}

void freeBuffer()
{
	free(sb->chars);
}

/*** EDITOR CONFIGURATIONS AND SETUP + OPERATIONS ***/
typedef struct Line
{
	int len, rlen;
	char *chars, *rchars;
} Line;

struct
{
	int cursor_x, cursor_y, render_x;
	int rows, cols;
	DWORD orig_in_mode, orig_out_mode;
	Line *lines;
	int num_lines;
	int row_offset, col_offset;
	char *filename;
	char status[128];
	time_t status_time;
	bool dirty;
} editor = {.cursor_x = 0, .render_x = 0, .cursor_y = 0, .lines = NULL, .num_lines = 0, .row_offset = 0, .col_offset = 0, .filename = NULL, .status[0] = '\0', .status_time = 0, .dirty = false};

void cursorToRenderX(Line *line)
{
	for (int i = 0; i < editor.cursor_x; i++)
	{
		if (line->chars[i] == '\t')
			editor.render_x += TAB_LENGTH - 1 - editor.render_x % TAB_LENGTH;
		editor.render_x++;
	}
}

void updateLine(Line *line)
{
	int tabs = 0;
	for (int i = 0; i < line->len; i++)
		if (line->chars[i] == '\t')
			tabs++;

	free(line->rchars);
	// subtract 1 from tab length since the escape character is already accounted for
	line->rchars = malloc(line->len + tabs * (TAB_LENGTH - 1) + 1);
	int index = 0;
	for (int i = 0; i < line->len; i++)
	{
		if (line->chars[i] == '\t')
		{
			do
			{
				line->rchars[index++] = ' ';
			} while (index % TAB_LENGTH);
		}
		else
			line->rchars[index++] = line->chars[i];
	}
	line->rchars[index] = '\0';
	line->rlen = index;
}

void insertLine(int index, char *str, size_t len)
{
	if (index < 0 || index > editor.num_lines)
		return;
	// allocate space for an extra line
	if((editor.lines = realloc(editor.lines, sizeof(Line) * (editor.num_lines + 1))) == NULL)
		die("Failed to insert line");

	memmove(&editor.lines[index + 1], &editor.lines[index], sizeof(Line) * (editor.num_lines - index));
	editor.lines[index] = (Line){len, 0, malloc(len + 1), NULL};
	memcpy(editor.lines[index].chars, str, len);
	editor.lines[index].chars[len] = '\0';

	updateLine(&editor.lines[index]);
	editor.num_lines++;
}

void insertNewline()
{
	if (editor.cursor_x == 0)
		insertLine(editor.cursor_y, "", 0);
	else
	{
		Line *line = &editor.lines[editor.cursor_y];
		insertLine(editor.cursor_y + 1, &line->chars[editor.cursor_x], line->len - editor.cursor_x);
		line = &editor.lines[editor.cursor_y];
		line->len = editor.cursor_x;
		line->chars[line->len] = '\0';
		updateLine(line);
	}

	editor.cursor_x = 0;
	editor.cursor_y++;
	editor.dirty = true;
}

void writeLines()
{
	for (int i = 0; i < editor.rows; i++)
	{
		int currentLine = i + editor.row_offset;
		if (currentLine >= editor.num_lines)
		{
			appendToBuffer("~", 1); // typical editor filler
		}
		else
		{
			int len = editor.lines[currentLine].rlen - editor.col_offset;
			clamp(&len, 0, editor.cols);
			appendToBuffer(&editor.lines[currentLine].rchars[editor.col_offset], len);
		}
		appendToBuffer("\x1b[K", 3);
		appendToBuffer("\r\n", 2);
	}
}

void editorBar()
{
	appendToBuffer("\x1b[7m", 4);
	char buffer[256], position[32]; // need buffer to be large since it will contain all the spaces as well
	const char* untitled = "[Untitled]";
	char *name = editor.filename ? strdup(editor.filename) : strdup(untitled);
	if (editor.dirty)
		strcat(name, "*");
	int len = snprintf(position, sizeof(position), "Line: %d/%d, Col %d/%d", editor.cursor_y + 1, editor.num_lines, editor.cursor_x, editor.cursor_y < editor.num_lines ? editor.lines[editor.cursor_y].len : 0);
	len = snprintf(buffer, sizeof(buffer), "%-*.20s", editor.cols - len, name);
	appendToBuffer(buffer, len);
	appendToBuffer(position, strlen(position));
	appendToBuffer("\x1b[m\r\n", 5);
}

// only shows for fives seconds or until user inputs a key after five seconds
void statusBar()
{
	appendToBuffer("\x1b[K", 3);
	int len = clamp(&(int){strlen(editor.status)}, 0, editor.cols);
	if (len && time(NULL) - editor.status_time < 5)
		appendToBuffer(editor.status, len);
}

// reusable function for when we need to display a message to the user
void setStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);										  // fmt is needed since it is the rightmost argument preceding our variable arguments
	vsnprintf(editor.status, sizeof(editor.status), fmt, ap); // no need for va_arg since it is handled in this function
	va_end(ap);
	editor.status_time = time(NULL); // gives current time - number of seconds since midnight Jan. 1st 1970
}

char *prompt(char *prompt)
{
	int size = 128;
	char *str = malloc(size);
	int len = 0;
	str[0] = '\0';
	while (1)
	{
		setStatusMessage(prompt, str);
		refreshScreen();
		int c = readKey();
		if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
			if (len != 0)
				str[--len] = '\0';
		}
		else if (c == ESCAPE_KEY)
		{
			setStatusMessage("");
			free(str);
			return NULL;
		}
		else if (c == ENTER_KEY)
		{
			if (len != 0)
			{
				setStatusMessage("");
				return str;
			}
		}
		else if (!iscntrl(c) && c < 128)
		{
			if (len == size - 1)
			{
				size *= 2;
				str = realloc(str, size);
			}
			str[len++] = c;
			str[len] = '\0';
		}
	}
}

/*** EDITOR INPUT ***/

// need positions for search and replace functionality later on
void lineInsert(Line *line, int index, char c)
{
	if (index < 0 || index > line->len)
		index = line->len;

	line->chars = realloc(line->chars, line->len + 2);
	memmove(&line->chars[index + 1], &line->chars[index], line->len - index + 1); // + 1 to move null terminator

	line->len++;
	line->chars[index] = c;
	updateLine(line);
	editor.dirty = true;
}

void insert(char c)
{
	if (editor.cursor_y == editor.num_lines)
		insertLine(editor.num_lines, "", 0);
	lineInsert(&editor.lines[editor.cursor_y], editor.cursor_x++, c);
}

void lineDelete(Line *line, int index)
{
	if (index < 0 || index > line->len)
		return;
	memmove(&line->chars[index], &line->chars[index + 1], line->len-- - index);
	updateLine(line);
	editor.dirty = true;
}

void deleteRow(int index)
{
	if (index < 0 || index > editor.num_lines)
		return;
	free(editor.lines[index].chars);
	free(editor.lines[index].rchars);

	memmove(&editor.lines[index], &editor.lines[index + 1], sizeof(Line) * (--editor.num_lines - index));
	editor.dirty = true;
}

void appendToLine(Line *line, char *str, int len)
{
	line->chars = realloc(line->chars, line->len + len + 1);
	memcpy(&line->chars[line->len], str, len);
	line->len += len;
	line->chars[line->len] = '\0';
	updateLine(line);
	editor.dirty = true; // TODO come back later to delete if unnecessary
}

void delete()
{
	if (!editor.cursor_x && !editor.cursor_y)
		return;
	if (editor.cursor_x > 0)
		lineDelete(&editor.lines[editor.cursor_y], --editor.cursor_x);
	else
	{
		editor.cursor_y--;
		editor.cursor_x = editor.lines[editor.cursor_y].len;
		appendToLine(&editor.lines[editor.cursor_y], editor.lines[editor.cursor_y + 1].chars, editor.lines[editor.cursor_y + 1].len);
		deleteRow(editor.cursor_y + 1);
	}
}

// BASIC EDITOR FUNCTIONS
char *ErrorExit()
{
	DWORD err = GetLastError();
	LPVOID msg;

	FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		err,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPSTR)&msg,
		0,
		NULL);
	char *buffer;
	sprintf(buffer, "GetLastError() - %lu: %s\n", err, (char *)msg);
	LocalFree(msg);
	return buffer;
}

void die(const char *s, ...)
{
	va_list args;
	va_start(args, s);
	char message[128];
	vsprintf(message, s, args); // error code from console
	free(s);
	va_end(args);
	resetScreen();

	HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	WriteConsoleA(stdOut, "\x1b[?1049l", 8, NULL, NULL);
	if (!SetConsoleMode(stdIn, editor.orig_in_mode) || !SetConsoleMode(stdOut, editor.orig_out_mode))
		printf("%ld - ", GetLastError());
	strcat(message, "\n");
	perror(message);
	printf(ErrorExit());
	exit(1);
}

void getWindowSize()
{
	CONSOLE_SCREEN_BUFFER_INFO console_info;
	if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &console_info))
		die("Error on getting window size");

	editor.cols = console_info.srWindow.Right - console_info.srWindow.Left + 1;
	editor.rows = console_info.srWindow.Bottom - console_info.srWindow.Top + 1; // y increases as we go down so bottom minus top

	editor.rows -= 2;
}

void disableRawMode()
{
	HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	WriteConsoleA(stdOut, "\x1b[?1049l", 8, NULL, NULL);
	if (!SetConsoleMode(stdIn, editor.orig_in_mode) || !SetConsoleMode(stdOut, editor.orig_out_mode))
		die("DisableRawMode(): Error on setting console mode.");
}

void enableRawMode()
{
	// tcgetattr
	HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!GetConsoleMode(stdIn, &editor.orig_in_mode) || !GetConsoleMode(stdOut, &editor.orig_out_mode))
		die("EnableRawMode(): Error on getting console mode.");
	atexit(disableRawMode);

	DWORD rawIn = editor.orig_in_mode;
	DWORD rawOut = editor.orig_out_mode;
	// line input
	rawIn &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
	// input processing
	rawIn |= ENABLE_EXTENDED_FLAGS;
	rawIn &= ~ENABLE_QUICK_EDIT_MODE;
	// output processing
	rawOut |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING; // ViRTUAL_TERMINAL_PROCESSING allows ANSI escape characters

	if (!SetConsoleMode(stdIn, rawIn) || !SetConsoleMode(stdOut, rawOut))
		die("EnableRawMode(): Error on setting console mode.");
}

void init()
{
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	WriteConsoleA(stdOut, "\x1b[?1049h", 8, NULL, NULL);
	resetScreen();
	getWindowSize();
}

/*** FILE I/O ***/
// have to make our own getline function since the original is for POSIX
SSIZE_T getline(char **lineptr, size_t *lenptr, FILE *stream)
{
	if (lineptr == NULL || lenptr == NULL || stream == NULL)
		return -1;
	char *buffer = *lineptr;
	size_t len = *lenptr;
	if (*lineptr == NULL || *lenptr == 0)
	{
		// make temp variables in case malloc doesn't work
		// original values should remain if function fails
		len = 128;
		buffer = malloc(len);
		if (!buffer)
			return -1;
		*lineptr = buffer;
		*lenptr = len;
	}
	size_t i = 0;
	for (int c = fgetc(stream); c != '\n'; c = fgetc(stream))
	{
		// this will let us make the end condition for the loop that collects all lines
		if (c == EOF)
		{
			if (i == 0)
				return -1;
			break;
		}

		if (i >= len)
		{
			len *= 2; // double length for resizing
			char *newBuffer = realloc(buffer, len);
			if (!newBuffer)
				return -1;
			*lineptr = buffer = newBuffer;
			*lenptr = len;
		}

		buffer[i++] = (char)c;
	}
	buffer[i] = '\0';
	return i;
}

void openEditor(char *filename)
{
	free(editor.filename);
	editor.filename = strdup(filename);
	FILE *file = fopen(filename, "r");
	if (!file)
		die("Could not open file");

	char *line = NULL;
	size_t len = 0;
	SSIZE_T llen; // line length
	while ((llen = getline(&line, &len, file)) != -1)
	{
		while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r')) // reduce size so string will be truncated when '\0' is inserted at llen
			llen--;
		insertLine(editor.num_lines, line, llen);
	}

	free(line);
	fclose(file);
}

void saveToDisk()
{
	if (!editor.filename)
	{
		editor.filename = prompt("Save As: %s");
		if (editor.filename == NULL)
		{
			setStatusMessage("Save aborted");
			return;
		}
	}
	int len = 0;
	for (int i = 0; i < editor.num_lines; i++)
		len += editor.lines[i].len + 1; // add one for new line

	char *full_text = malloc(len);
	char *ptr = full_text;
	for (int i = 0; i < editor.num_lines; i++)
	{
		memcpy(ptr, editor.lines[i].chars, editor.lines[i].len);
		ptr += editor.lines[i].len; // go to last index
		*ptr = '\n';				// append new line
		ptr++;						// append from after this
	}
	*ptr = '\0';
	// die(full_text);
	//  int *file_handle;
	//  if(_sopen_s(&file_handle, editor.filename, _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE))
	//  	die("Couldn't save to Disk");
	HANDLE file_handle = CreateFileA(editor.filename, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file_handle == INVALID_HANDLE_VALUE)
	{
		CloseHandle(file_handle);
		die("Couldn't write to disk - File couldn't be opened");
	}
	LARGE_INTEGER li;
	li.QuadPart = len;
	if (!SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN))
	{
		free(full_text);
		CloseHandle(file_handle);
		setStatusMessage(ErrorExit());
		return;
	}

	if (!SetEndOfFile(file_handle))
	{
		free(full_text);
		CloseHandle(file_handle);
		setStatusMessage(ErrorExit());
		return;
	}

	li.QuadPart = 0;
	if (!SetFilePointerEx(file_handle, li, NULL, FILE_BEGIN))
	{
		free(full_text);
		CloseHandle(file_handle);
		setStatusMessage(ErrorExit());
		return;
	}

	if (!WriteFile(file_handle, full_text, len, NULL, NULL))
	{
		free(full_text);
		CloseHandle(file_handle);
		setStatusMessage(ErrorExit());
		return;
	}

	free(full_text);
	CloseHandle(file_handle);
	editor.dirty = false;
	setStatusMessage("Wrote %d bytes to file: %s", len, editor.filename);
}

/*** BASIC I/O ***/

// move cursor with arrow keys
void moveCursor(int key)
{
	Line *current = editor.cursor_y >= editor.num_lines ? NULL : &editor.lines[editor.cursor_y];
	switch (key)
	{
	case ARROW_LEFT:
		if (editor.cursor_x > 0)
			editor.cursor_x--;
		else if (editor.cursor_y > 0)
		{
			editor.cursor_y--;
			editor.cursor_x = !current ? 0 : current->len;
		}
		break;
	case ARROW_RIGHT:
		if (current && editor.cursor_x < current->len) // null check before accessing member value
			editor.cursor_x++;
		else if (current && editor.cursor_x == current->len && editor.cursor_y < editor.num_lines - 1)
		{
			editor.cursor_y++;
			editor.cursor_x = 0;
		}
		break;
	case ARROW_UP:
		if (editor.cursor_y > 0)
			editor.cursor_y--;
		break;
	case ARROW_DOWN:
		if (editor.cursor_y < editor.num_lines - 1) // we can keep -1 to keep it looking good, or 0 so we can insert on the last line
			editor.cursor_y++;
		break;
	}

	// keep cursor from going past the end of a line
	current = editor.cursor_y >= editor.num_lines ? NULL : &editor.lines[editor.cursor_y];
	int len = current ? current->len : 0;
	editor.cursor_x = editor.cursor_x > len ? len : editor.cursor_x;
}

void scroll()
{
	editor.render_x = 0;
	if (editor.cursor_y < editor.num_lines)
	{
		cursorToRenderX(&editor.lines[editor.cursor_y]);
	}

	clamp(&editor.row_offset, editor.cursor_y - editor.rows + 1, editor.cursor_y);
	clamp(&editor.col_offset, editor.render_x - editor.cols + 1, editor.render_x);
}

void refreshScreen()
{
	scroll();
	sb = SB_INIT;
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);

	appendToBuffer("\x1b[H", 3);
	writeLines();
	editorBar();
	statusBar();

	char buf[32];
	// terminal is 1-indexed, hence the plus one for cursor position
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", editor.cursor_y - editor.row_offset + 1, editor.render_x - editor.col_offset + 1);
	appendToBuffer(buf, strlen(buf));
	WriteConsoleA(stdOut, sb->chars, sb->len, NULL, NULL);
	freeBuffer();
}

// read one character from the console
int readKey()
{
	int read;

	HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD record;
	DWORD len = 0;

	// read will be nonzero if no error and zero for error
	while (1)
	{
		DWORD wait = WaitForSingleObject(stdIn, 100);
		if (wait == WAIT_OBJECT_0)
		{
			if ((read = ReadConsoleInputA(stdIn, &record, 1, &len)))
			{
				if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
				{
					KEY_EVENT_RECORD keyEvent = record.Event.KeyEvent;
					switch (keyEvent.wVirtualKeyCode)
					{
					case VK_CONTROL:
					case VK_LCONTROL:
					case VK_RCONTROL:
					case VK_SHIFT:
					case VK_LSHIFT:
					case VK_RSHIFT:
						continue;
					case VK_LEFT:
						return ARROW_LEFT;
					case VK_RIGHT:
						return ARROW_RIGHT;
					case VK_UP:
						return ARROW_UP;
					case VK_DOWN:
						return ARROW_DOWN;
					case VK_PRIOR:
						return PAGE_UP;
					case VK_NEXT:
						return PAGE_DOWN;
					case VK_HOME:
						return HOME_KEY;
					case VK_END:
						return END_KEY;
					case VK_DELETE:
						return DELETE_KEY;
					case VK_BACK:
						return BACKSPACE;
					case VK_RETURN:
						return ENTER_KEY;
					case VK_ESCAPE:
						return ESCAPE_KEY;
					default:
						return keyEvent.uChar.AsciiChar;
					}
				}
				if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
				{
					getWindowSize();
					refreshScreen();
					continue;
				}
			}
			if (!read && errno != EAGAIN)
				die("Read Key");
		}
		else if (wait == WAIT_TIMEOUT)
			len = 0;
		else
			die("Some other error");
	}
	return '\0';
}

void processKeypress()
{
	static int quit_left = QUIT_CONFIRMATION; // static so value persists after next key press
	int c = readKey();
	switch (c)
	{
	case CTRL_KEY('q'):
		if (editor.dirty && quit_left)
		{
			setStatusMessage("WARNING! File has unsaved changes. Press CTRL-Q %d more times to confirm.", quit_left--);
			return;
		}
		resetScreen();
		exit(0);
		break;
	case CTRL_KEY('s'):
		saveToDisk();
		break;
	case ARROW_LEFT:
	case ARROW_RIGHT:
	case ARROW_UP:
	case ARROW_DOWN:
		moveCursor(c);
		break;
	case PAGE_UP:
	case PAGE_DOWN:
		int n = editor.rows;
		while (n--)
			moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		break;
	case HOME_KEY:
	case END_KEY:
		n = editor.cursor_y >= editor.num_lines ? editor.cols : editor.lines[editor.cursor_y].len;
		while (n--)
		{
			if (c == HOME_KEY && editor.cursor_x > 0)
				moveCursor(ARROW_LEFT);
			else if (c == END_KEY && editor.cursor_y < editor.num_lines && editor.cursor_x < editor.lines[editor.cursor_y].len)
				moveCursor(ARROW_RIGHT);
		}
		break;
	case DELETE_KEY:
		int current_x = editor.cursor_x;
		moveCursor(ARROW_RIGHT);
		if(current_x != editor.cursor_x)
			delete();
		break;
	case BACKSPACE:
		delete();
		break;
	case ENTER_KEY:
		insertNewline();
		break;
	default:
		insert(c);
		break;
	}
	quit_left = QUIT_CONFIRMATION;
}

int main(int argc, char const *argv[])
{
	HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	enableRawMode();
	init();

	// user provided filename
	if (argc > 1)
		openEditor(argv[1]);

	setStatusMessage("CTRL-Q To Quit - Asterisk (*) means file has been modified since last save");
	while (1)
	{
		// doesnt really do anything, cant tell if it works or not
		/*
		const char* cursorOn = "\x1b[?25l";
		const char* cursorOff = "\x1b[?25h";


		WriteConsoleA(stdOut, cursorOn, (DWORD)strlen(cursorOn), NULL, NULL);
		WriteConsoleA(stdOut, cursorOff, (DWORD)strlen(cursorOff), &num, NULL);
		*/
		refreshScreen();
		processKeypress();
	}
	return 0;
}