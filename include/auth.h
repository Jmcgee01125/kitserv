/* Part of FileKit, licensed under the GNU Affero GPL. */

#ifndef AUTH_H
#define AUTH_H

/**
 * Initialize user authentication system.
 * Returns 0 on success, -1 if authentication failed.
 * (If this fails, authentication will always return "not logged in").
 */
int auth_init(int exp_time);

#endif
