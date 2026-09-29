// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Badge, type BadgeTone } from '../../ui';
import { useConnectionState } from '../connection';
import styles from './TopBar.module.css';

const TONES: Record<ReturnType<typeof useConnectionState>, BadgeTone> = {
  live: 'success',
  connecting: 'warning',
  offline: 'neutral',
};

/**
 * Whether live updates are flowing (G-arch 5: `EventSource` on the `…/watch`
 * routes).
 *
 * The wording carries the state, not just the colour: a green dot alone is
 * invisible to a screen reader and meaningless to a user with a colour-vision
 * deficiency. `role="status"` so a drop to "offline" is announced once, without
 * interrupting.
 */
export function ConnectionIndicator() {
  const { t } = useTranslation('shell');
  const status = useConnectionState();
  const state = t(`connection.${status}`);

  return (
    // `role="status"` takes its name from the author, not from its content, so
    // the state is spelled out in `aria-label` as well as shown in the badge:
    // that is what makes this element findable and announceable by name.
    <span className={styles.connection} role="status" aria-label={t('connection.label', { state })}>
      <Badge tone={TONES[status]}>{state}</Badge>
    </span>
  );
}
