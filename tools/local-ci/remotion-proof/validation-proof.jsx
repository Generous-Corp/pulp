import React from 'react';
import {
	AbsoluteFill,
	Img,
	Sequence,
	Video,
	interpolate,
	staticFile,
	useCurrentFrame,
} from 'remotion';

const colors = {
	ink: '#172033',
	panel: '#f8fafc',
	line: '#cbd5e1',
	accent: '#0f766e',
	warn: '#9a3412',
	muted: '#64748b',
};

const byteLabel = (bytes) => {
	if (typeof bytes !== 'number') {
		return 'not measured';
	}
	if (bytes >= 1_000_000) {
		return `${(bytes / 1_000_000).toFixed(1)} MB`;
	}
	return `${Math.max(1, Math.round(bytes / 1000))} KB`;
};

const shortSha = (sha) => {
	if (!sha || typeof sha !== 'string') {
		return null;
	}
	return sha.slice(0, 12);
};

const pillText = (value, hasMeasuredSize = false) => {
	if (value === true) {
		return hasMeasuredSize ? 'Source clip under budget' : 'Issue attachment ready';
	}
	if (value === false) {
		return 'Needs hosted fallback';
	}
	return hasMeasuredSize ? 'Source clip measured' : 'Attachment check pending';
};

const issueStatusLabel = (status, selectedAttempt) => {
	if (!status) {
		return null;
	}
	if (status === 'copied') {
		return 'Issue attachment ready';
	}
	if (status === 'transcoded') {
		return selectedAttempt
			? `Issue attachment ready (${selectedAttempt})`
			: 'Issue attachment ready';
	}
	if (status === 'oversized') {
		return 'Needs hosted fallback';
	}
	return `${status}${selectedAttempt ? ` (${selectedAttempt})` : ''}`;
};

const clamp = (value, min, max) => Math.max(min, Math.min(max, value));

const proofStyle = {
	fontFamily:
		'-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif',
	background:
		'linear-gradient(135deg, #f8fafc 0%, #eef6f5 48%, #fff7ed 100%)',
	color: colors.ink,
};

export const ValidationProof = ({
	title,
	subtitle,
	template,
	videoFileName,
	videoHasAudio,
	posterFileName,
	sourceImageFileName,
	sourceLabel,
	target,
	action,
	label,
	completedAt,
	interactionMode,
	sourceMode,
	sourceSha,
	sourceBranch,
	captureMode,
	durationSecs,
	fps,
	sizeBytes,
	attachmentBudgetBytes,
	fitsAttachmentBudget,
	issueStatus,
	issueSelectedAttempt,
	imageChanged,
	focus,
	actionMarker,
	contextItems,
	stepItems,
	notes,
}) => {
	const frame = useCurrentFrame();
	const introOpacity = interpolate(frame, [0, 24, 54], [0, 1, 0], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const contentOpacity = interpolate(frame, [36, 64], [0, 1], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const noteItems = Array.isArray(notes) ? notes.slice(0, 4) : [];
	const steps = Array.isArray(stepItems) ? stepItems.slice(0, 4) : [];
	const contextRows = Array.isArray(contextItems) ? contextItems.slice(0, 6) : [];
	const designParity = template === 'design-parity' && sourceImageFileName;
	const componentZoom = template === 'component-zoom';
	const focusCenter = focus?.normalized_center || {x: 0.5, y: 0.5};
	const focusSize = focus?.normalized_size || {width: 0.26, height: 0.24};
	const focusBox = {
		left: `${clamp((focusCenter.x || 0.5) - (focusSize.width || 0.26) / 2, 0.04, 0.86) * 100}%`,
		top: `${clamp((focusCenter.y || 0.5) - (focusSize.height || 0.24) / 2, 0.06, 0.82) * 100}%`,
		width: `${clamp(focusSize.width || 0.26, 0.14, 0.48) * 100}%`,
		height: `${clamp(focusSize.height || 0.24, 0.12, 0.42) * 100}%`,
	};
	const focusLabel = focus?.label || 'Selected component';
	const focusPulse = interpolate(frame, [70, 92, 116, 138], [0.85, 1, 0.88, 1], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const markerPoint = actionMarker?.normalized_point || null;
	const markerPulse = interpolate(frame, [74, 88, 112, 138], [0, 1, 0.35, 0], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const markerScale = interpolate(frame, [74, 92, 138], [0.62, 1.18, 1], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const focusScale = interpolate(frame, [64, 100, 170], [1, 1.18, 1.18], {
		extrapolateLeft: 'clamp',
		extrapolateRight: 'clamp',
	});
	const issueText =
		issueStatusLabel(issueStatus, issueSelectedAttempt) ||
		pillText(fitsAttachmentBudget, typeof sizeBytes === 'number');
	const sourceText = [sourceMode, shortSha(sourceSha), sourceBranch]
		.filter(Boolean)
		.join(' / ');
	const captureText = [
		captureMode,
		typeof durationSecs === 'number' ? `${durationSecs}s` : null,
		typeof fps === 'number' ? `${Math.round(fps)} fps` : null,
	]
		.filter(Boolean)
		.join(' / ');

	return (
		<AbsoluteFill style={proofStyle}>
			<AbsoluteFill
				style={{
					padding: 40,
					opacity: contentOpacity,
					display: 'grid',
					gridTemplateColumns: designParity ? '1fr 1fr 356px' : '1fr 392px',
					gap: 26,
				}}
			>
				{designParity ? (
					<div
						style={{
							border: `1px solid ${colors.line}`,
							borderRadius: 14,
							background: '#f8fafc',
							boxShadow: '0 24px 64px rgba(15, 23, 42, 0.14)',
							overflow: 'hidden',
							position: 'relative',
						}}
					>
						<Img
							src={staticFile(sourceImageFileName)}
							style={{
								width: '100%',
								height: '100%',
								objectFit: 'contain',
							}}
						/>
						<div
							style={{
								position: 'absolute',
								left: 18,
								top: 18,
								padding: '10px 14px',
								borderRadius: 999,
								background: 'rgba(15, 23, 42, 0.76)',
								color: '#f8fafc',
								fontSize: 20,
							}}
						>
							{sourceLabel || 'Source reference'}
						</div>
					</div>
				) : null}
				<div
					style={{
						border: `1px solid ${colors.line}`,
						borderRadius: 14,
						background: '#020617',
						boxShadow: '0 24px 64px rgba(15, 23, 42, 0.22)',
						overflow: 'hidden',
						position: 'relative',
					}}
				>
					{videoFileName ? (
						<Video
							src={staticFile(videoFileName)}
							muted={!videoHasAudio}
							style={{
								width: '100%',
								height: '100%',
								objectFit: 'contain',
							}}
						/>
					) : posterFileName ? (
						<Img
							src={staticFile(posterFileName)}
							style={{
								width: '100%',
								height: '100%',
								objectFit: 'contain',
							}}
						/>
					) : (
						<div
							style={{
								display: 'flex',
								alignItems: 'center',
								justifyContent: 'center',
								height: '100%',
								color: '#e2e8f0',
								fontSize: 30,
							}}
						>
							No visual artifact
						</div>
					)}
					<div
						style={{
							position: 'absolute',
							left: 18,
							bottom: 18,
							padding: '10px 14px',
							borderRadius: 999,
							background: 'rgba(15, 23, 42, 0.76)',
							color: '#f8fafc',
							fontSize: 20,
						}}
					>
						{target}/{action}
					</div>
					{imageChanged !== null ? (
						<div
							style={{
								position: 'absolute',
								right: 18,
								top: 18,
								padding: '9px 12px',
								borderRadius: 999,
								background: imageChanged
									? 'rgba(20, 83, 45, 0.78)'
									: 'rgba(71, 85, 105, 0.78)',
								color: '#f8fafc',
								fontSize: 18,
								fontWeight: 760,
							}}
						>
							{imageChanged ? 'Visual change detected' : 'No visual diff'}
						</div>
					) : null}
					{componentZoom ? (
						<>
							<div
								style={{
									position: 'absolute',
									...focusBox,
									border: '4px solid #2dd4bf',
									borderRadius: 18,
									boxShadow: `0 0 0 ${10 * focusPulse}px rgba(45, 212, 191, 0.18), 0 18px 42px rgba(15, 23, 42, 0.28)`,
								}}
							/>
							<div
								style={{
									position: 'absolute',
									left: 22,
									top: 22,
									padding: '10px 14px',
									borderRadius: 12,
									background: 'rgba(15, 118, 110, 0.88)',
									color: '#f8fafc',
									fontSize: 19,
									fontWeight: 780,
								}}
							>
								Focus: {focusLabel}
							</div>
							<div
								style={{
									position: 'absolute',
									right: 20,
									bottom: 20,
									width: 284,
									height: 166,
									border: '3px solid rgba(255,255,255,0.92)',
									borderRadius: 16,
									background: '#020617',
									boxShadow: '0 20px 54px rgba(2, 6, 23, 0.48)',
									overflow: 'hidden',
								}}
							>
								<div
									style={{
										position: 'absolute',
										inset: 0,
										transform: `scale(${focusScale})`,
										transformOrigin: `${clamp(focusCenter.x || 0.5, 0.05, 0.95) * 100}% ${clamp(focusCenter.y || 0.5, 0.05, 0.95) * 100}%`,
									}}
								>
									{videoFileName ? (
										<Video
											src={staticFile(videoFileName)}
											muted
											style={{width: '100%', height: '100%', objectFit: 'contain'}}
										/>
									) : posterFileName ? (
										<Img
											src={staticFile(posterFileName)}
											style={{width: '100%', height: '100%', objectFit: 'contain'}}
										/>
									) : null}
								</div>
								<div
									style={{
										position: 'absolute',
										left: 12,
										top: 10,
										color: '#f8fafc',
										fontSize: 16,
										fontWeight: 760,
										textShadow: '0 1px 6px rgba(2, 6, 23, 0.8)',
									}}
								>
									zoom detail
								</div>
							</div>
						</>
					) : null}
					{markerPoint ? (
						<div
							style={{
								position: 'absolute',
								left: `${clamp(markerPoint.x, 0.03, 0.97) * 100}%`,
								top: `${clamp(markerPoint.y, 0.04, 0.96) * 100}%`,
								transform: `translate(-50%, -50%) scale(${markerScale})`,
								opacity: 0.2 + markerPulse * 0.8,
								width: 54,
								height: 54,
								borderRadius: 999,
								border: '5px solid #f97316',
								boxShadow: `0 0 0 ${22 * markerPulse}px rgba(249, 115, 22, 0.24), 0 10px 28px rgba(15, 23, 42, 0.32)`,
								background: 'rgba(255, 247, 237, 0.72)',
							}}
						>
							<div
								style={{
									position: 'absolute',
									left: '50%',
									top: '50%',
									width: 12,
									height: 12,
									borderRadius: 999,
									background: '#ea580c',
									transform: 'translate(-50%, -50%)',
								}}
							/>
						</div>
					) : null}
				</div>
				<div
					style={{
						border: `1px solid ${colors.line}`,
						borderRadius: 14,
						background: 'rgba(255,255,255,0.86)',
						padding: 26,
						boxShadow: '0 18px 48px rgba(15, 23, 42, 0.14)',
					}}
				>
					<div style={{fontSize: 21, color: colors.muted, marginBottom: 8}}>
						{subtitle}
					</div>
					<div style={{fontSize: 39, lineHeight: 1.05, fontWeight: 760}}>
						{title}
					</div>
					<div
						style={{
							marginTop: 20,
							padding: '11px 13px',
							borderRadius: 10,
							background: fitsAttachmentBudget === false ? '#fff7ed' : '#ecfdf5',
							border:
								fitsAttachmentBudget === false
									? '1px solid #fed7aa'
									: '1px solid #99f6e4',
							color: fitsAttachmentBudget === false ? colors.warn : '#115e59',
							fontSize: 18,
							fontWeight: 760,
						}}
					>
						{issueText}
					</div>
					<div style={{marginTop: 22, display: 'grid', gap: 10}}>
						{steps.map((step, index) => (
							<div
								key={`${step.label}-${index}`}
								style={{
									display: 'grid',
									gridTemplateColumns: '32px 1fr',
									gap: 11,
									alignItems: 'start',
								}}
							>
								<div
									style={{
										width: 28,
										height: 28,
										borderRadius: 999,
										background: colors.ink,
										color: '#fff',
										display: 'flex',
										alignItems: 'center',
										justifyContent: 'center',
										fontSize: 15,
										fontWeight: 800,
									}}
								>
									{index + 1}
								</div>
								<div>
									<div style={{fontSize: 17, fontWeight: 800}}>{step.label}</div>
									<div style={{fontSize: 16, color: colors.muted, lineHeight: 1.22}}>
										{step.detail}
									</div>
								</div>
							</div>
						))}
					</div>
					<div
						style={{
							marginTop: 22,
							paddingTop: 18,
							borderTop: `1px solid ${colors.line}`,
							display: 'grid',
							gap: 10,
							fontSize: 17,
						}}
					>
						<div>
							<strong>Run</strong>
							<br />
							{label}
						</div>
						{interactionMode ? (
							<div>
								<strong>Interaction</strong>
								<br />
								{interactionMode}
							</div>
						) : null}
						{contextRows.length ? (
							<div>
								<strong>Context</strong>
								<br />
								<div style={{display: 'grid', gap: 4, marginTop: 4}}>
									{contextRows.map((item) => (
										<div
											key={item.key}
											style={{
												display: 'grid',
												gridTemplateColumns: '92px 1fr',
												gap: 8,
												alignItems: 'baseline',
											}}
										>
											<span style={{color: colors.muted}}>{item.key}</span>
											<span>{item.value}</span>
										</div>
									))}
								</div>
							</div>
						) : null}
						{sourceText ? (
							<div>
								<strong>Source</strong>
								<br />
								{sourceText}
							</div>
						) : null}
						{captureText ? (
							<div>
								<strong>Capture</strong>
								<br />
								{captureText}
							</div>
						) : null}
						<div style={{display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12}}>
							<div>
								<strong>File size</strong>
								<br />
								{byteLabel(sizeBytes)}
							</div>
							<div>
								<strong>Budget</strong>
								<br />
								{byteLabel(attachmentBudgetBytes)}
							</div>
						</div>
						{completedAt ? (
							<div>
								<strong>Completed</strong>
								<br />
								{completedAt}
							</div>
						) : null}
					</div>
					{noteItems.length ? (
						<div
							style={{
								marginTop: 20,
								paddingTop: 16,
								borderTop: `1px solid ${colors.line}`,
								display: 'grid',
								gap: 7,
								fontSize: 15,
								color: colors.muted,
							}}
						>
							{noteItems.map((note, index) => (
								<div key={index}>- {note}</div>
							))}
						</div>
					) : null}
				</div>
			</AbsoluteFill>
			<Sequence durationInFrames={64}>
				<AbsoluteFill
					style={{
						opacity: introOpacity,
						justifyContent: 'center',
						alignItems: 'center',
						padding: 70,
						textAlign: 'center',
					}}
				>
					<div
						style={{
							fontSize: 28,
							color: colors.accent,
							fontWeight: 760,
							marginBottom: 18,
						}}
					>
						Pulp validation proof
					</div>
					<div style={{fontSize: 70, lineHeight: 1.02, fontWeight: 820}}>
						{title}
					</div>
					<div style={{fontSize: 30, marginTop: 18, color: colors.muted}}>
						{target}/{action} - {label}
					</div>
				</AbsoluteFill>
			</Sequence>
		</AbsoluteFill>
	);
};
