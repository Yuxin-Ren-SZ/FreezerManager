// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams } from 'react-router-dom';
import { apiErrorMessage } from '../../api/helpers';
import { ErrorState, Spinner } from '../../ui';
import { SampleForm } from './SampleForm';
import styles from './SampleForm.module.css';
import { useSampleReferenceData } from './useSampleReferenceData';

/**
 * `New sample` (TODO.md G3.3) — the create half of the sample screen.
 *
 * The form itself is `SampleForm`, shared with the detail view's edit mode so
 * the generated field list, the validation mirror and the server-rejection
 * handling exist once. What is specific to create is where a success goes: the
 * new sample's own detail page, rather than back to the form.
 *
 * The reference data is the same composition the detail view uses, so opening
 * this screen after a detail view costs no extra requests.
 */
export function SampleCreateScreen() {
  const { t } = useTranslation('sample-detail');
  const { t: tCommon } = useTranslation();
  const navigate = useNavigate();
  const labId = useParams().labId ?? '';
  const reference = useSampleReferenceData(labId);

  const onSaved = useCallback(
    (sample: { id: string }) => {
      void navigate(`/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(sample.id)}`);
    },
    [labId, navigate],
  );

  if (reference.isPending) {
    return <Spinner />;
  }

  if (reference.isError) {
    return (
      <section className={styles.screen}>
        <ErrorState
          title={t('errorTitle')}
          description={apiErrorMessage(tCommon, reference.error)}
          onRetry={() => {
            void reference.refetch();
          }}
        />
      </section>
    );
  }

  return (
    <section className={styles.screen}>
      <SampleForm
        labId={labId}
        sample={null}
        reference={reference}
        onSaved={onSaved}
        onCancel={() => {
          void navigate(`/labs/${encodeURIComponent(labId)}/samples`);
        }}
      />
    </section>
  );
}
