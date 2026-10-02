// Écrit à la main (remplace IlmThreadConfig.h) : initialisations protégées pour un usage multi-threads
#ifndef INCLUDED_ILMTHREAD_CONFIG_H
#define INCLUDED_ILMTHREAD_CONFIG_H 1
#pragma once

#define ILMTHREAD_THREADING_ENABLED 1
#define ILMTHREAD_HAVE_POSIX_SEMAPHORES 0
#define ILMTHREAD_USE_TBB 0

#define ILMTHREAD_INTERNAL_NAMESPACE_CUSTOM 0
#define ILMTHREAD_INTERNAL_NAMESPACE IlmThread_3_5
#define ILMTHREAD_NAMESPACE_CUSTOM 0
#define ILMTHREAD_NAMESPACE IlmThread

#define ILMTHREAD_DEPRECATED(msg) __declspec(deprecated (msg))
#endif
