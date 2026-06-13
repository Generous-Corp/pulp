#!/usr/bin/env node
import {bundle} from '@remotion/bundler';
import {renderMedia, selectComposition} from '@remotion/renderer';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const toolDir = path.resolve(scriptDir, '..');
const entryPoint = path.join(toolDir, 'remotion-proof', 'index.jsx');

const usage = () => {
	console.error(`usage: compose-video-proof.mjs --manifest <manifest.json> --output <proof.mp4> [--video <raw.mp4>] [--title <title>] [--template validation-proof|design-parity] [--source-image <png>] [--source-label <label>] [--note <text>]...`);
	process.exit(2);
};

const parseArgs = (argv) => {
	const args = {};
	for (let index = 0; index < argv.length; index += 1) {
		const arg = argv[index];
		if (!arg.startsWith('--')) {
			usage();
		}
		const key = arg.slice(2);
		const value = argv[index + 1];
		if (!value || value.startsWith('--')) {
			usage();
		}
		if (key === 'note') {
			args.note = [...(args.note || []), value];
		} else {
			args[key] = value;
		}
		index += 1;
	}
	return args;
};

const copyIfPresent = async (source, publicDir, name) => {
	if (!source) {
		return null;
	}
	const resolved = path.resolve(source);
	await fs.access(resolved);
	const ext = path.extname(resolved) || path.extname(name);
	const fileName = `${path.basename(name, path.extname(name))}${ext}`;
	await fs.copyFile(resolved, path.join(publicDir, fileName));
	return fileName;
};

const readJsonIfPresent = async (source) => {
	if (!source) {
		return {};
	}
	try {
		return JSON.parse(await fs.readFile(path.resolve(source), 'utf8'));
	} catch {
		return {};
	}
};

const firstPresentSelector = (selector) => {
	if (!selector || typeof selector !== 'object') {
		return null;
	}
	for (const key of ['id', 'label', 'text', 'type', 'click_view_id', 'click_view_label', 'click_view_text', 'click_view_type', 'point']) {
		if (selector[key]) {
			return `${key}: ${selector[key]}`;
		}
	}
	return null;
};

const actionMarkerLabel = (marker) => {
	if (!marker || typeof marker !== 'object') {
		return null;
	}
	if (marker.label) {
		return `${marker.kind || 'action'}: ${marker.label}`;
	}
	const point = marker.content_point || marker.normalized_point;
	if (point && typeof point === 'object') {
		return `${marker.kind || 'action'} at ${Math.round(point.x)},${Math.round(point.y)}`;
	}
	return marker.kind || null;
};

const contextItemsFor = (context) => {
	if (!context || typeof context !== 'object') {
		return [];
	}
	return Object.entries(context)
		.filter(([, value]) => value !== null && value !== undefined && `${value}`.trim())
		.map(([key, value]) => ({key, value: `${value}`}))
		.slice(0, 8);
};

const stepItemsFor = (manifest, videoMeta, issueMeta, proofNotes = []) => {
	const interaction = manifest.interaction || {};
	const selectorLabel = firstPresentSelector(interaction.click?.selector);
	const focusLabel = manifest.video_proof_composition?.focus?.label;
	const markerLabel = actionMarkerLabel(manifest.video_proof_composition?.action_marker);
	const proofContext = manifest.video_proof_composition?.context || {};
	const pluginHost = [proofContext.host, proofContext.plugin, proofContext.format]
		.filter(Boolean)
		.join(' / ');
	const clickPoint = interaction.click?.content_point || interaction.click?.screen_point;
	const clickDetail = selectorLabel || (clickPoint ? `point: ${Math.round(clickPoint.x)},${Math.round(clickPoint.y)}` : null);
	const actionDetail = interaction.mode
		? `${interaction.mode}${clickDetail ? ` -> ${clickDetail}` : ''}`
		: proofNotes.length
			? proofNotes.slice(0, 2).join(' ')
			: 'no interaction recorded';
	const captureDetail = [
		videoMeta.mode || 'video proof',
		videoMeta.duration_secs ? `${videoMeta.duration_secs}s` : null,
		videoMeta.fps ? `${Math.round(videoMeta.fps)} fps` : null,
	]
		.filter(Boolean)
		.join(' / ');
	const reviewDetail = issueMeta.status
		? `${issueMeta.status}${issueMeta.selected_attempt ? ` (${issueMeta.selected_attempt})` : ''}`
		: videoMeta.size?.fits_attachment_budget === false
			? 'needs hosted fallback'
			: 'issue attachment ready';
	if (focusLabel) {
		return [
			{
				label: 'Launch',
				detail: pluginHost || `${manifest.target || 'target'}/${manifest.action || 'run'}`,
			},
			{
				label: 'Focus',
				detail: `${focusLabel}${clickDetail ? ` via ${clickDetail}` : ''}`,
			},
			{
				label: 'Action',
				detail: `${markerLabel || actionDetail}${captureDetail ? ` / ${captureDetail}` : ''}`,
			},
			{
				label: 'Review',
				detail: reviewDetail,
			},
		];
	}
	return [
		{
			label: 'Launch',
			detail: pluginHost || `${manifest.target || 'target'}/${manifest.action || 'run'}`,
		},
		{
			label: proofNotes.length && !interaction.mode ? 'Evidence' : 'Action',
			detail: markerLabel || actionDetail,
		},
		{
			label: 'Capture',
			detail: captureDetail,
		},
		{
			label: 'Review',
			detail: reviewDetail,
		},
	];
};

const main = async () => {
	const args = parseArgs(process.argv.slice(2));
	if (!args.manifest || !args.output) {
		usage();
	}
	const manifestPath = path.resolve(args.manifest);
	const outputPath = path.resolve(args.output);
	const manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
	const artifacts = manifest.artifacts || {};
	const rawVideo = args.video || artifacts.video;
	const poster = artifacts.video_poster || artifacts.screenshot;
	const template = args.template || manifest.video_proof_composition?.template || 'validation-proof';
	const sourceImage =
		args['source-image'] ||
		manifest.video_proof_composition?.source_image ||
		null;
	const sourceLabel =
		args['source-label'] ||
		manifest.video_proof_composition?.source_label ||
		'Source reference';
	const videoMetaPath = artifacts.video_metadata;
	const videoMeta = await readJsonIfPresent(videoMetaPath);
	const issueMeta = await readJsonIfPresent(artifacts.video_issue_metadata);
	const proofNotes = [
		...(Array.isArray(manifest.video_proof_notes) ? manifest.video_proof_notes : []),
		...(Array.isArray(manifest.video_proof_composition?.notes) ? manifest.video_proof_composition.notes : []),
		...(Array.isArray(args.note) ? args.note : []),
	]
		.filter((note) => typeof note === 'string' && note.trim())
		.map((note) => note.trim())
		.filter((note, index, notes) => notes.indexOf(note) === index);

	const tempRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'pulp-remotion-proof-'));
	const publicDir = path.join(tempRoot, 'public');
	const bundleDir = path.join(tempRoot, 'bundle');
	await fs.mkdir(publicDir, {recursive: true});
	await fs.mkdir(path.dirname(outputPath), {recursive: true});

		try {
			const videoFileName = await copyIfPresent(rawVideo, publicDir, 'raw-video.mp4');
			const posterFileName = await copyIfPresent(poster, publicDir, 'poster.png');
			const sourceImageFileName = await copyIfPresent(sourceImage, publicDir, 'source-reference.png');
			const inputProps = {
				title: args.title || manifest.video_proof_composition?.title || manifest.label || 'Validation Proof',
				subtitle:
					template === 'design-parity'
						? 'Design parity proof'
						: template === 'plugin-host'
							? 'Plugin host proof'
							: template === 'inspector-workflow'
								? 'Inspector workflow proof'
								: template === 'standalone'
									? 'Standalone app proof'
									: 'Desktop automation proof',
				template,
				videoFileName,
				posterFileName,
				sourceImageFileName,
				sourceLabel,
				target: manifest.target || 'unknown',
				action: manifest.action || 'run',
				label: manifest.label || 'untitled',
				completedAt: manifest.completed_at || null,
				interactionMode: manifest.interaction?.mode || null,
				sourceMode: manifest.source?.mode || null,
				sourceSha: manifest.source?.sha || null,
				sourceBranch: manifest.source?.branch || null,
				captureMode: videoMeta.mode || null,
				durationSecs: videoMeta.duration_secs ?? null,
				fps: videoMeta.fps ?? null,
				sizeBytes: videoMeta.size?.size_bytes ?? null,
				attachmentBudgetBytes: videoMeta.size?.attachment_budget_bytes ?? null,
				fitsAttachmentBudget:
					videoMeta.size?.fits_attachment_budget ?? null,
				issueStatus: issueMeta.status || null,
				issueSelectedAttempt: issueMeta.selected_attempt || null,
				imageChanged: artifacts.image_change?.changed ?? null,
				focus: manifest.video_proof_composition?.focus || null,
				actionMarker: manifest.video_proof_composition?.action_marker || null,
				proofContext: manifest.video_proof_composition?.context || {},
				contextItems: contextItemsFor(manifest.video_proof_composition?.context),
				stepItems: stepItemsFor(manifest, videoMeta, issueMeta, proofNotes),
				notes: [
					...proofNotes,
					manifest.source?.mode ? `source: ${manifest.source.mode}` : null,
					manifest.source?.sha ? `sha: ${manifest.source.sha.slice(0, 12)}` : null,
					videoMeta.mode ? `capture: ${videoMeta.mode}` : null,
					videoMeta.frame_count ? `${videoMeta.frame_count} captured frames` : null,
					issueMeta.status ? `issue variant: ${issueMeta.status}` : null,
				].filter(Boolean),
			};

		const serveUrl = await bundle({
			entryPoint,
			outDir: bundleDir,
			publicDir,
			enableCaching: false,
			ignoreRegisterRootWarning: true,
		});
		const composition = await selectComposition({
			serveUrl,
			id: 'ValidationProof',
			inputProps,
			logLevel: 'warn',
		});
		await renderMedia({
			composition,
			serveUrl,
			codec: 'h264',
			outputLocation: outputPath,
			inputProps,
			overwrite: true,
			muted: true,
			audioCodec: null,
			crf: 23,
			x264Preset: 'veryfast',
			logLevel: 'warn',
		});
		const stat = await fs.stat(outputPath);
		console.log(
			JSON.stringify(
					{
						output: outputPath,
						size_bytes: stat.size,
						composer: 'remotion',
						composition_id: 'ValidationProof',
						input_summary: {
							title: inputProps.title,
							template: inputProps.template,
							target: inputProps.target,
							action: inputProps.action,
							step_count: inputProps.stepItems.length,
							note_count: proofNotes.length,
							issue_status: inputProps.issueStatus,
						},
					},
					null,
					2,
				),
		);
	} finally {
		await fs.rm(tempRoot, {recursive: true, force: true});
	}
};

main().catch((error) => {
	console.error(error?.stack || String(error));
	process.exit(1);
});
