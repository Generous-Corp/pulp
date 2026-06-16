#!/usr/bin/env node
import {spawn} from 'node:child_process';
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import ffmpegStaticPath from 'ffmpeg-static';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const toolDir = path.resolve(scriptDir, '..');
const outputRoot = path.join(toolDir, '.video-proof-smoke');
const rawVideoPath = path.join(outputRoot, 'raw-proof.mp4');
	const posterPath = path.join(outputRoot, 'poster.png');
	const metadataPath = path.join(outputRoot, 'metadata.json');
	const issueMetadataPath = path.join(outputRoot, 'issue-metadata.json');
	const manifestPath = path.join(outputRoot, 'manifest.json');
const composedPath = path.join(outputRoot, 'proof-composed.mp4');
const composerPath = path.join(scriptDir, 'compose-video-proof.mjs');

const run = (command, args) =>
	new Promise((resolve, reject) => {
		const child = spawn(command, args, {
			stdio: ['ignore', 'pipe', 'pipe'],
		});
		let stdout = '';
		let stderr = '';
		child.stdout.on('data', (chunk) => {
			stdout += chunk;
		});
		child.stderr.on('data', (chunk) => {
			stderr += chunk;
		});
		child.on('error', reject);
		child.on('close', (code) => {
			if (code === 0) {
				resolve({stdout, stderr});
				return;
			}
			const tail = (stderr || stdout || `exit ${code}`).slice(-1000);
			reject(new Error(`${command} ${args.join(' ')} failed: ${tail}`));
		});
	});

const runInspect = (command, args) =>
	new Promise((resolve, reject) => {
		const child = spawn(command, args, {
			stdio: ['ignore', 'pipe', 'pipe'],
		});
		let stdout = '';
		let stderr = '';
		child.stdout.on('data', (chunk) => {
			stdout += chunk;
		});
		child.stderr.on('data', (chunk) => {
			stderr += chunk;
		});
		child.on('error', reject);
		child.on('close', (code) => {
			resolve({code, stdout, stderr});
		});
	});

const ffmpegPath = () => {
	const configured = process.env.PULP_FFMPEG || process.env.PULP_FFMPEG_PATH || process.env.FFMPEG_PATH;
	return configured || ffmpegStaticPath;
};

const main = async () => {
	const ffmpeg = ffmpegPath();
	if (!ffmpeg) {
		throw new Error('ffmpeg not found; run npm install or set PULP_FFMPEG.');
	}
	await fs.rm(outputRoot, {recursive: true, force: true});
	await fs.mkdir(outputRoot, {recursive: true});
	await run(ffmpeg, [
		'-hide_banner',
		'-y',
		'-f',
		'lavfi',
		'-i',
		'testsrc=size=960x540:rate=30',
		'-t',
		'2',
		'-an',
		'-c:v',
		'libx264',
		'-preset',
		'veryfast',
		'-crf',
		'23',
		'-pix_fmt',
		'yuv420p',
		'-movflags',
		'+faststart',
		rawVideoPath,
	]);
	await run(ffmpeg, [
		'-hide_banner',
		'-y',
		'-i',
		rawVideoPath,
		'-frames:v',
		'1',
		posterPath,
	]);
	const rawStat = await fs.stat(rawVideoPath);
	await fs.writeFile(
		metadataPath,
		JSON.stringify(
			{
				kind: 'desktop-video-proof',
				path: rawVideoPath,
				duration_secs: 2,
				fps: 30,
				has_audio: false,
				audio_source: 'none',
				mode: 'synthetic-smoke',
					size: {
						exists: true,
						size_bytes: rawStat.size,
						attachment_budget_bytes: 100_000_000,
						fits_attachment_budget: rawStat.size <= 100_000_000,
					},
					bounds: {x: 0, y: 0, width: 960, height: 540},
				},
				null,
				2,
			) + '\n',
		);
		await fs.writeFile(
			issueMetadataPath,
			JSON.stringify(
				{
					kind: 'desktop-video-proof-issue-variant',
					source: rawVideoPath,
					output: composedPath,
					status: 'transcoded',
					selected_attempt: 'balanced-720p',
					size: {
						exists: true,
						size_bytes: rawStat.size,
						attachment_budget_bytes: 100_000_000,
						fits_attachment_budget: true,
					},
				},
				null,
				2,
			) + '\n',
		);
		await fs.writeFile(
			manifestPath,
			JSON.stringify(
				{
					target: 'synthetic',
					action: 'video-smoke',
					label: 'Remotion smoke proof',
					completed_at: new Date().toISOString(),
					artifacts: {
						video: rawVideoPath,
						video_poster: posterPath,
						video_metadata: metadataPath,
						video_issue_metadata: issueMetadataPath,
						image_change: {changed: true, mode: 'synthetic'},
					},
					interaction: {
						mode: 'synthetic',
						click: {
							selector: {id: 'proof-toggle'},
							content_point: {x: 480, y: 270},
						},
					},
					video_proof_composition: {
						context: {
							recipe: 'synthetic-smoke',
							host: 'local-ci',
							component: 'proof-toggle',
						},
						action_marker: {
							kind: 'click',
							label: 'proof-toggle',
							content_point: {x: 480, y: 270},
							normalized_point: {x: 0.5, y: 0.5},
						},
					},
					source: {
						mode: 'local-ci-video-smoke',
						sha: '0123456789abcdef0123456789abcdef01234567',
						branch: 'feat/validation-video-proof',
					},
				},
				null,
				2,
			) + '\n',
	);
	await run(process.execPath, [
		composerPath,
		'--manifest',
		manifestPath,
		'--output',
		composedPath,
		'--title',
		'Remotion smoke proof',
	]);
	const composedInfo = await runInspect(ffmpeg, ['-hide_banner', '-i', composedPath]);
	if (/Stream #.*Audio:/i.test(`${composedInfo.stderr}\n${composedInfo.stdout}`)) {
		throw new Error('composed proof unexpectedly contains an audio stream');
	}
	const composedStat = await fs.stat(composedPath);
	console.log(
		JSON.stringify(
			{
				output: composedPath,
				size_bytes: composedStat.size,
				raw_video: rawVideoPath,
				manifest: manifestPath,
			},
			null,
			2,
		),
	);
};

main().catch((error) => {
	console.error(error?.stack || String(error));
	process.exit(1);
});
