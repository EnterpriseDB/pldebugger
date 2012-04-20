/*
 * dbgcomm.h
 *
 * This file defines the functions used to establish connections between
 * the debugging proxy and target backend.
 *
 * Copyright (c) 2012 EnterpriseDB Corporation. All Rights Reserved.
 *
 * Licensed under the Artistic License, see 
 *		http://www.opensource.org/licenses/artistic-license.php
 * for full details
 */
#ifndef DBGCOMM_H
#define DBGCOMM_H

extern void dbgcomm_reserve(void);

extern int dbgcomm_connect_to_proxy(int proxyPort);
extern int dbgcomm_listen_for_proxy(void);

extern int dbgcomm_listen_for_target(int *port);
extern int dbgcomm_accept_target(int sockfd, int *targetPid);
extern int dbgcomm_connect_to_target(BackendId targetBackend);

#endif
