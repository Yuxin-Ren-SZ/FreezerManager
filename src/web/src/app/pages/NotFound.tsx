// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { Button, EmptyState } from '../../ui';

/**
 * Rendered by the router's `*` route, inside the shell: a wrong address should
 * still leave the user with a working navigation, not a dead end.
 */
export function NotFound() {
  const { t } = useTranslation('shell');
  const navigate = useNavigate();

  return (
    <EmptyState
      title={t('notFound.title')}
      description={t('notFound.body')}
      action={
        <Button
          variant="primary"
          onClick={() => {
            void navigate('/');
          }}
        >
          {t('notFound.home')}
        </Button>
      }
    />
  );
}
