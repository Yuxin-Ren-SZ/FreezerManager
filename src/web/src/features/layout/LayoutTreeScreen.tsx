// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage } from '../../api/helpers';
import { EmptyState, ErrorState, Spinner } from '../../ui';
import { LayoutTree } from './LayoutTree';
import styles from './LayoutTreeScreen.module.css';
import { useLabLayout } from './useLabLayout';

/**
 * Storage layout (TODO.md G3.1) — the lab's freezers, containers and boxes as a
 * collapsible tree.
 *
 * It replaces the G1.3 placeholder that used to sit at this route: the route,
 * its TODO id and its declared `sample.read` permission in
 * `src/app/route-map.tsx` are unchanged, and the file is the one G-arch 11
 * reserved for this feature.
 *
 * One `useLabLayout(labId)` call is the whole data layer. The screen's job is
 * to show exactly one of four states, and never to mix them:
 *
 *  - loading, until all four lists have answered;
 *  - **error**, if any of them failed — a half-built tree must not be shown,
 *    because freezers with nothing under them look exactly like an empty lab;
 *  - empty, when the lab genuinely has no freezers;
 *  - the tree.
 *
 * `PERMISSION_DENIED` lands in the error branch like any other failure (G-arch
 * 8: the server is the enforcement point, and the screen has to handle the
 * refusal rather than assume its route guard was enough).
 */
export function LayoutTreeScreen() {
  const { t } = useTranslation('layout');
  // `errors.*` lives in the default namespace, so the translated sentence for a
  // failure needs that `t`, not the layout one.
  const { t: tCommon } = useTranslation();
  const labId = useParams().labId ?? '';
  const { tree, isPending, isError, error, refetch } = useLabLayout(labId);
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set<string>());

  const onToggle = useCallback((nodeId: string) => {
    setCollapsed((current) => {
      const next = new Set(current);
      if (next.has(nodeId)) {
        next.delete(nodeId);
      } else {
        next.add(nodeId);
      }
      return next;
    });
  }, []);

  return (
    <section className={styles.screen}>
      <h1 className={styles.title}>{t('title')}</h1>

      {isPending ? <Spinner /> : null}

      {!isPending && isError ? (
        <ErrorState
          title={t('tree.errorTitle')}
          description={apiErrorMessage(tCommon, error)}
          requestId={isApiError(error) ? (error.requestId ?? undefined) : undefined}
          onRetry={() => {
            void refetch();
          }}
        />
      ) : null}

      {!isPending && !isError && tree.length === 0 ? (
        <EmptyState title={t('tree.emptyTitle')} description={t('tree.emptyHint')} />
      ) : null}

      {!isPending && !isError && tree.length > 0 ? (
        <div className={styles.panel}>
          <LayoutTree nodes={tree} labId={labId} collapsed={collapsed} onToggle={onToggle} />
        </div>
      ) : null}
    </section>
  );
}
