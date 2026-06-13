import React from 'react';
import {Composition, registerRoot} from 'remotion';
import {ValidationProof} from './validation-proof.jsx';

const defaultProps = {
	title: 'Validation Proof',
	subtitle: 'Desktop automation',
	template: 'validation-proof',
	videoFileName: null,
	posterFileName: null,
	sourceImageFileName: null,
	sourceLabel: 'Source reference',
	target: 'unknown',
	action: 'run',
	label: 'untitled',
	completedAt: null,
	interactionMode: null,
	sourceMode: null,
	sourceSha: null,
	sourceBranch: null,
	captureMode: null,
	durationSecs: null,
	fps: null,
	sizeBytes: null,
	attachmentBudgetBytes: null,
	fitsAttachmentBudget: null,
	issueStatus: null,
	issueSelectedAttempt: null,
	imageChanged: null,
	focus: null,
	actionMarker: null,
	proofContext: {},
	contextItems: [],
	stepItems: [],
	notes: [],
};

const Root = () => {
	return (
		<Composition
			id="ValidationProof"
			component={ValidationProof}
			durationInFrames={240}
			fps={30}
			width={1280}
			height={720}
			defaultProps={defaultProps}
		/>
	);
};

registerRoot(Root);
