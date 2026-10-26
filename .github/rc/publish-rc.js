const fs = require('fs');
const crypto = require('crypto');

module.exports = async ({ github, context, core }) => {
  const config = JSON.parse(fs.readFileSync('.github/rc/candidate.json', 'utf8'));
  const tag = config.tag;
  const body = fs.readFileSync('.github/rc/release-notes.md', 'utf8');
  const title = `${config.component} — October 2026 RC1`;
  const files = fs.readdirSync('rc-assets').sort();
  if (!files.includes('SHA256SUMS') || !files.includes('RC_MANIFEST.json')) {
    throw new Error('Incomplete release payload');
  }

  // Tags are immutable candidate identities. Resolve annotated tags before
  // comparing, and never move an existing tag or replace published assets.
  let ref;
  try {
    ({ data: ref } = await github.rest.git.getRef({ ...context.repo, ref: `tags/${tag}` }));
  } catch (error) {
    if (error.status !== 404) throw error;
  }
  if (ref) {
    let object = ref.object;
    while (object.type === 'tag') {
      const { data: annotated } = await github.rest.git.getTag({ ...context.repo, tag_sha: object.sha });
      object = annotated.object;
    }
    if (object.type !== 'commit' || object.sha !== config.source_sha) {
      throw new Error('Existing RC tag points at a different commit');
    }
  } else {
    const { data: annotated } = await github.rest.git.createTag({
      ...context.repo, tag, object: config.source_sha, type: 'commit',
      message: `${title}\n\nValidated source: ${config.source_sha}\nBuild: ${config.build_run}\n`
    });
    await github.rest.git.createRef({ ...context.repo, ref: `refs/tags/${tag}`, sha: annotated.sha });
  }

  const releases = await github.paginate(github.rest.repos.listReleases, { ...context.repo, per_page: 100 });
  let release = releases.find(item => item.tag_name === tag);
  if (!release) {
    ({ data: release } = await github.rest.repos.createRelease({
      ...context.repo, tag_name: tag, target_commitish: config.source_sha,
      name: title, body, draft: true, prerelease: true, make_latest: 'false'
    }));
  }
  if (!release.prerelease || release.name !== title || release.body !== body) {
    throw new Error('Existing release metadata differs from this candidate');
  }
  const existing = await github.paginate(github.rest.repos.listReleaseAssets, {
    ...context.repo, release_id: release.id, per_page: 100
  });
  if (existing.some(asset => !files.includes(asset.name))) {
    throw new Error('Existing candidate contains unexpected assets');
  }
  for (const name of files) {
    const data = fs.readFileSync(`rc-assets/${name}`);
    const digest = `sha256:${crypto.createHash('sha256').update(data).digest('hex')}`;
    const asset = existing.find(item => item.name === name);
    if (asset) {
      if (asset.size !== data.length || asset.digest !== digest) {
        throw new Error(`Existing asset differs: ${name}`);
      }
    } else {
      if (!release.draft) throw new Error('Published release is incomplete; refusing to alter it');
      await github.rest.repos.uploadReleaseAsset({
        ...context.repo, release_id: release.id, name, data,
        headers: { 'content-type': 'application/octet-stream', 'content-length': data.length }
      });
    }
  }
  const uploaded = await github.paginate(github.rest.repos.listReleaseAssets, {
    ...context.repo, release_id: release.id, per_page: 100
  });
  if (uploaded.length !== files.length || uploaded.some(asset => asset.state !== 'uploaded')) {
    throw new Error('Release assets are incomplete; leaving the release as a draft');
  }
  for (const asset of uploaded) {
    const data = fs.readFileSync(`rc-assets/${asset.name}`);
    const digest = `sha256:${crypto.createHash('sha256').update(data).digest('hex')}`;
    if (asset.size !== data.length || asset.digest !== digest) {
      throw new Error(`Uploaded asset failed digest validation: ${asset.name}`);
    }
  }
  if (release.draft) {
    ({ data: release } = await github.rest.repos.updateRelease({
      ...context.repo, release_id: release.id, draft: false,
      prerelease: true, make_latest: 'false'
    }));
  }
  await core.summary.addHeading(title).addLink('Download RC1', release.html_url)
    .addRaw(`\n\nSource: ${config.source_sha}\nBuild: ${config.build_run}\n`).write();
  core.notice(`Published prerelease: ${release.html_url}`);
};
