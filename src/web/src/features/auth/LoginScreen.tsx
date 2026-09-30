// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState, type SyntheticEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { Navigate, useLocation, useNavigate } from 'react-router-dom';
import { signIn } from '../../api/auth';
import { isApiError } from '../../api/errors';
import { useSession } from '../../app/session';
import { Button, TextField } from '../../ui';
import { mfaPath, safeNext } from './next';
import styles from './auth.module.css';

/**
 * Sign in — G2.1, against G0.1's existing browser route.
 *
 * Three things this screen is careful about:
 *
 * 1. **It never sees a token.** `POST /api/v1/auth/browser/login` answers with
 *    `Set-Cookie: fmgr_session=…; HttpOnly` and no token in the body, so there
 *    is nothing here to put in `localStorage` (G-arch 6, AGENTS.md §5). What it
 *    reads from the response is one boolean.
 * 2. **A wrong password is not an expired session.** `src/api/client.ts` reports
 *    every `UNAUTHENTICATED` to the session-expired listeners, and
 *    `SessionExpiryWatcher` ignores them while nobody is signed in — otherwise a
 *    refused login would tear the form out from under the user.
 * 3. **`?next=` is validated** (`safeNext`) before it is navigated to: it comes
 *    from the URL, so following it unvalidated is an open redirect.
 */
export function LoginScreen() {
  const { t } = useTranslation(['auth', 'shell']);
  const { status, refresh } = useSession();
  const location = useLocation();
  const navigate = useNavigate();

  const next = safeNext(location.search);
  const expired = new URLSearchParams(location.search).has('expired');

  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // A signed-in visitor has no business at the password form — including the
  // one who just signed in: this is what carries them on to `next`.
  if (status === 'authenticated') {
    return <Navigate to={next} replace />;
  }

  function describe(cause: unknown): string {
    if (isApiError(cause)) {
      if (cause.code === 'UNAVAILABLE' || cause.code === 'DEADLINE_EXCEEDED') {
        return t('auth:unreachable');
      }
      if (cause.code === 'RESOURCE_EXHAUSTED') {
        return t('auth:tooManyAttempts');
      }
      if (cause.code === 'UNAUTHENTICATED' || cause.code === 'PERMISSION_DENIED') {
        // `LocalAuthProvider` refuses an unknown email and a wrong password
        // identically, and an account locked after repeated failures arrives as
        // `PERMISSION_DENIED`. One message for all three: the difference is not
        // the user's to learn here, and saying which half was wrong is what
        // would make this form an account-enumeration oracle.
        return t('auth:invalidCredentials');
      }
    }
    return t('auth:failed');
  }

  async function handleSubmit(event: SyntheticEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault();
    setBusy(true);
    setError(null);

    try {
      const result = await signIn(email, password);
      if (result.mfaRequired) {
        // The cookie is already set; the second factor is what is missing (#62).
        void navigate(mfaPath(next), { replace: true });
        return;
      }

      // The cookie is the session, so who the user is now comes from asking
      // again. The loader is the `auth/whoami` call the shell was given.
      const reloaded = await refresh();
      if (reloaded.status !== 'authenticated') {
        setError(reloaded.status === 'error' ? t('auth:unreachable') : t('auth:failed'));
      }
    } catch (cause) {
      setError(describe(cause));
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className={styles.page}>
      <div className={styles.card}>
        <p className={styles.brand}>{t('shell:app.name')}</p>
        <h1 className={styles.title}>{t('auth:title')}</h1>

        {expired ? (
          <p className={styles.notice} role="status">
            {t('auth:mfa.expired')}
          </p>
        ) : null}

        {error !== null ? (
          <p className={styles.alert} role="alert">
            {error}
          </p>
        ) : null}

        <form className={styles.form} onSubmit={(event) => void handleSubmit(event)}>
          <TextField
            label={t('auth:email')}
            type="email"
            name="email"
            autoComplete="username"
            required
            value={email}
            onChange={(event) => {
              setEmail(event.target.value);
            }}
          />
          <TextField
            label={t('auth:password')}
            type="password"
            name="password"
            autoComplete="current-password"
            required
            value={password}
            onChange={(event) => {
              setPassword(event.target.value);
            }}
          />
          <Button type="submit" variant="primary" block loading={busy}>
            {busy ? t('auth:submitting') : t('auth:submit')}
          </Button>
        </form>
      </div>
    </main>
  );
}
