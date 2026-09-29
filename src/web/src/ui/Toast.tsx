// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { IconButton } from './IconButton';
import styles from './Toast.module.css';
import { classNames } from './classNames';

/** Not a translatable string: it is a glyph, and the name comes from `label`. */
const DISMISS_GLYPH = '\u00d7';

export type ToastTone = 'info' | 'success' | 'warning' | 'danger';

export interface ToastOptions {
  title: string;
  description?: string;
  tone?: ToastTone;
  /** Milliseconds on screen; `0` keeps the toast until it is dismissed. */
  duration?: number;
}

export interface ToastApi {
  show: (options: ToastOptions) => string;
  dismiss: (id: string) => void;
}

interface ToastRecord extends ToastOptions {
  id: string;
}

const ToastContext = createContext<ToastApi | null>(null);

const DEFAULT_DURATION_MS = 5000;

export interface ToastProviderProps {
  children: ReactNode;
  /** Overrides the 5 s default; useful in tests. */
  defaultDuration?: number;
}

/**
 * Toast host (TODO.md G1.3: "toasts").
 *
 * The provider owns both the queue and the live region, so a toast raised from
 * a component that is about to unmount — the usual case for "Saved" — still
 * gets announced.
 *
 * Severity picks the live-region politeness: `danger` is `role="alert"`
 * (assertive, interrupts), everything else is `role="status"` (polite, waits
 * for a pause). Getting that backwards either buries failures or makes every
 * success message interrupt the screen reader mid-sentence.
 */
export function ToastProvider({
  children,
  defaultDuration = DEFAULT_DURATION_MS,
}: ToastProviderProps) {
  const [toasts, setToasts] = useState<ToastRecord[]>([]);
  const timers = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  const nextId = useRef(0);
  const { t } = useTranslation('ui');

  const dismiss = useCallback((id: string) => {
    const timer = timers.current.get(id);
    if (timer !== undefined) {
      clearTimeout(timer);
      timers.current.delete(id);
    }
    setToasts((current) => current.filter((toast) => toast.id !== id));
  }, []);

  const show = useCallback(
    (options: ToastOptions) => {
      nextId.current += 1;
      const id = `fmgr-toast-${String(nextId.current)}`;
      const duration = options.duration ?? defaultDuration;

      setToasts((current) => [...current, { ...options, id }]);

      if (duration > 0) {
        timers.current.set(
          id,
          setTimeout(() => {
            dismiss(id);
          }, duration),
        );
      }
      return id;
    },
    [defaultDuration, dismiss],
  );

  // A pending timer that fires after unmount would call setState on a dead
  // component; clear them all when the provider goes away.
  useEffect(() => {
    const pending = timers.current;
    return () => {
      for (const timer of pending.values()) {
        clearTimeout(timer);
      }
      pending.clear();
    };
  }, []);

  const api = useMemo<ToastApi>(() => ({ show, dismiss }), [show, dismiss]);

  return (
    <ToastContext.Provider value={api}>
      {children}
      <div className={styles.viewport} role="region" aria-label={t('toast.region')}>
        {toasts.map((toast) => (
          <ToastItem
            key={toast.id}
            toast={toast}
            dismissLabel={t('toast.dismiss')}
            onDismiss={dismiss}
          />
        ))}
      </div>
    </ToastContext.Provider>
  );
}

export function useToast(): ToastApi {
  const api = useContext(ToastContext);
  if (api === null) {
    throw new Error('useToast() must be called inside a <ToastProvider>');
  }
  return api;
}

function ToastItem({
  toast,
  dismissLabel,
  onDismiss,
}: {
  toast: ToastRecord;
  dismissLabel: string;
  onDismiss: (id: string) => void;
}) {
  const tone = toast.tone ?? 'info';

  return (
    <div
      className={classNames(styles.root, styles[tone])}
      role={tone === 'danger' ? 'alert' : 'status'}
    >
      <div className={styles.text}>
        <p className={styles.title}>{toast.title}</p>
        {toast.description ? <p className={styles.description}>{toast.description}</p> : null}
      </div>
      <IconButton
        label={dismissLabel}
        size="sm"
        onClick={() => {
          onDismiss(toast.id);
        }}
      >
        {DISMISS_GLYPH}
      </IconButton>
    </div>
  );
}
