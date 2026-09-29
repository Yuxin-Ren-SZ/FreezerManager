// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { Kbd } from '../../ui';
import styles from './TopBar.module.css';

function isTypingTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) {
    return false;
  }
  const tag = target.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || target.isContentEditable;
}

/**
 * The global lookup box, and the reason the shell listens for `/`.
 *
 * `/` is the shortcut the single-handed lookup flow (PRD §9, G3.5) is built
 * around; binding it here rather than in that screen means it works from
 * anywhere in the app. It does not fire while the user is typing (or the
 * shortcut would eat the character) and it does not fire with a modifier held
 * (Cmd-/ is the browser's own).
 */
export function GlobalLookup() {
  const { t } = useTranslation('shell');
  const navigate = useNavigate();
  const inputRef = useRef<HTMLInputElement>(null);
  const [query, setQuery] = useState('');

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key !== '/' || event.metaKey || event.ctrlKey || event.altKey) {
        return;
      }
      if (isTypingTarget(event.target)) {
        return;
      }
      event.preventDefault();
      inputRef.current?.focus();
      inputRef.current?.select();
    };

    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('keydown', onKeyDown);
    };
  }, []);

  return (
    <form
      role="search"
      className={styles.lookup}
      onSubmit={(event) => {
        event.preventDefault();
        const trimmed = query.trim();
        if (trimmed.length > 0) {
          // G-arch 7: URLs carry ids only. A search term is not an id, but it
          // is the user's own input and stays on the same origin, and the
          // lookup screen has to be linkable for the single-handed flow.
          void navigate(`/lookup?q=${encodeURIComponent(trimmed)}`);
        }
      }}
    >
      <input
        ref={inputRef}
        type="search"
        className={styles.lookupInput}
        aria-label={t('lookup.label')}
        aria-keyshortcuts="/"
        placeholder={t('lookup.placeholder')}
        value={query}
        onChange={(event) => {
          setQuery(event.target.value);
        }}
      />
      <span className={styles.shortcut} title={t('lookup.shortcut')}>
        <Kbd>/</Kbd>
      </span>
    </form>
  );
}
