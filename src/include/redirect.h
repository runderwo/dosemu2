/*
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

int RedirectDisk(int, char *, int);
int CancelDiskRedirection(int);
int ResetRedirection(int);
int GetRedirectionRoot(int,char **,int *);
void redirect_devices(void);
