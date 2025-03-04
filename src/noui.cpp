// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#include <string>
#include <cstdio>

#include "ui_interface.h"

static int noui_ThreadSafeMessageBox(const std::string &message,
  const std::string &caption, int style) {
    printf("%s: %s\n", caption.c_str(), message.c_str());
    fprintf(stderr, "%s: %s\n", caption.c_str(), message.c_str());
    return(4);
}

static bool noui_ThreadSafeAskFee(int64 nFeeRequired, const std::string &strCaption) {
    return(true);
}

void noui_connect() {
    /* Connect signal handlers */
    uiInterface.ThreadSafeMessageBox.connect(noui_ThreadSafeMessageBox);
    uiInterface.ThreadSafeAskFee.connect(noui_ThreadSafeAskFee);
}
