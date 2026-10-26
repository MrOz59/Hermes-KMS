const fs = require('fs');

module.exports = async ({ github, context, core }) => {
  const config = JSON.parse(fs.readFileSync('.github/rc/candidate.json', 'utf8'));
  if (`${context.repo.owner}/${context.repo.repo}` !== config.repository) {
    throw new Error('Candidate repository mismatch');
  }
  for (const run_id of [config.build_run, ...config.check_runs]) {
    const { data: run } = await github.rest.actions.getWorkflowRun({ ...context.repo, run_id });
    if (run.head_sha !== config.source_sha || run.status !== 'completed' ||
        run.conclusion !== 'success' || run.repository.full_name !== config.repository) {
      throw new Error(`Run ${run_id} does not validate the candidate commit`);
    }
  }
  const artifacts = await github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
    ...context.repo, run_id: config.build_run, per_page: 100
  });
  if (artifacts.length !== config.artifacts.length) {
    throw new Error('Build artifact set changed');
  }
  for (const expected of config.artifacts) {
    const artifact = artifacts.find(item => item.id === expected.id);
    if (!artifact || artifact.expired || artifact.name !== expected.name ||
        artifact.digest !== expected.digest ||
        artifact.workflow_run?.head_sha !== config.source_sha) {
      throw new Error(`Artifact ${expected.id} failed identity/provenance validation`);
    }
  }
  core.setOutput('run_id', String(config.build_run));
  core.notice(`Promoting validated ${config.component} commit ${config.source_sha}`);
};
