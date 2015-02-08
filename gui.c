#include <ncurses.h>

int main()
{
	int timecnt = 0;
	initscr();
	raw();
	noecho();
	keypad(stdscr, TRUE);

	timeout(0);
	mvprintw(10,10, "Johannes H. !!!");
	mvprintw(40,0, "kakkor=420");
	while(1)
	{
		int ch;
		timecnt++;
		usleep(1000);
		if(timecnt%1000 == 0)
			mvprintw(20,20, "%u", timecnt);
		ch = getch();
		if(ch == 'q')
			break;
		if(ch == KEY_RIGHT)
			printw("KAKKOR RIGHT");

		int jeah=5;
		mvscanw(40,0, "kakkor=%u", &jeah);
		mvprintw(41,10, "-->%u", jeah);
		refresh();
	}
	endwin();

	return 0;
}
