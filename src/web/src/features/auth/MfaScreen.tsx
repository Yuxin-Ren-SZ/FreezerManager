// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState, type SyntheticEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { Navigate, useLocation, useNavigate } from 'react-router-dom';
import { submitMfaCode } from '../../api/auth';
import { useSession } from '../../app/session';
import { Button, TextField } from '../../ui';
import { loginPath, safeNext } from './next';
import styles from './auth.module.css';

/** The TOTP code is six digits; the input says so as well as the server. */
const CODE_LENGTH = 6;

/**
 * Second factor — G2.1.
 *
 * This screen exists because of a deliberate G0.1 decision (#62): the browser
 * login route sets the session cookie *before* the TOTP code is entered, and
 * `AuthMiddleware` refuses every other RPC until it is. So a half-finished login
 * is a **real, resumable state**, not a dead end — a reload lands back here, and
 * the cookie is still the pending session.
 *
 * The subtle part is what a refusal means. `submit-mfa` answers
 * `UNAUTHENTICATED` for a wrong code, for a session that was already completed
 * and for a session that is gone; the status cannot tell them apart. So instead
 * of guessing, this screen asks the session what is true *now* — see the
 * `refresh()` call in the submit handler — and only then decides between "try
 * again", "you are already in" and "sign in again".
 */
export function MfaScreen() {
  const { t } = useTranslation(['auth', 'shell']);
  const { status, refresh, signOut } = useSession();
  const location = useLocation();
  const navigate = useNavigate();

  const next = safeNext(location.search);

  const [code, setCode] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // Already through the second factor — the code was accepted in another tab,
  // or the session never needed one. Either way there is nothing to enter.
  if (status === 'authenticated') {
    return <Navigate to={next} replace />;
  }

  async function handleSubmit(event: SyntheticEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault();
    setBusy(true);
    setError(null);

    // The refusal is kept, not thrown: what it *means* comes from asking the
    // session afterwards, and the answer decides between three outcomes.
    const refused = await submitMfaCode(code).then(
      () => null,
      (cause: unknown) => cause,
    );
    const reloaded = await refresh();

    if (reloaded.status === 'authenticated') {
      // The redirect above carries the user on; nothing more to do here.
      return;
    }
    if (reloaded.status === 'unauthenticated') {
      // The pending login is gone (abandoned long enough to expire, revoked, or
      // never accepted). Sending the user back to the form with the target
      // intact is the only useful thing left.
      void navigate(loginPath(next, { expired: true }), { replace: true });
      return;
    }
    if (refused !== null && reloaded.status === 'mfa-pending') {
      // Still pending, so the code was the problem — and the login survives it.
      setError(t('auth:mfa.invalidCode'));
    } else {
      setError(reloaded.status === 'error' ? t('auth:unreachable') : t('auth:failed'));
    }
    setBusy(false);
  }

  async function handleStartOver(): Promise<void> {
    setBusy(true);
    // #62: give the credential up rather than leaving a pending cookie behind.
    await signOut();
    void navigate('/login', { replace: true });
  }

  return (
    <main className={styles.page}>
      <div className={styles.card}>
        <p className={styles.brand}>{t('shell:app.name')}</p>
        <h1 className={styles.title}>{t('auth:mfa.title')}</h1>
        <p className={styles.prompt}>{t('auth:mfa.prompt')}</p>

        {error !== null ? (
          <p className={styles.alert} role="alert">
            {error}
          </p>
        ) : null}

        <form className={styles.form} onSubmit={(event) => void handleSubmit(event)}>
          <TextField
            label={t('auth:mfa.code')}
            name="totp"
            inputMode="numeric"
            autoComplete="one-time-code"
            pattern="[0-9]*"
            maxLength={CODE_LENGTH}
            required
            value={code}
            onChange={(event) => {
              setCode(event.target.value);
            }}
          />
          <div className={styles.actions}>
            <Button
              type="button"
              variant="ghost"
              onClick={() => void handleStartOver()}
              disabled={busy}
            >
              {t('auth:mfa.startOver')}
            </Button>
            <Button type="submit" variant="primary" loading={busy}>
              {busy ? t('auth:mfa.submitting') : t('auth:mfa.submit')}
            </Button>
          </div>
        </form>
      </div>
    </main>
  );
}
