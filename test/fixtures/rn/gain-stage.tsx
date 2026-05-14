import React, { useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

export default function GainStage() {
  const [armed, setArmed] = useState(true);
  const [gain, setGain] = useState(0.72);

  const decreaseGain = () => setGain(Math.max(0, Number((gain - 0.05).toFixed(2))));
  const increaseGain = () => setGain(Math.min(1, Number((gain + 0.05).toFixed(2))));
  const gainDb = Math.round((gain * 36 - 24) * 10) / 10;

  return (
    <View id="rn-gain-stage" testID="rn-gain-stage" style={styles.panel}>
      <View style={styles.header}>
        <View style={styles.titleBlock}>
          <Text style={styles.eyebrow}>React Native export</Text>
          <Text style={styles.title}>Gain Stage</Text>
        </View>
        <Pressable
          accessibilityLabel="Toggle bypass"
          onPress={() => setArmed(!armed)}
          style={styles.bypassButton}
        >
          <Text style={styles.bypassText}>{armed ? 'ARMED' : 'BYPASS'}</Text>
        </Pressable>
      </View>

      <View style={styles.meterRow}>
        <View style={styles.meterStack}>
          <View style={styles.barDim} />
          <View style={styles.barLow} />
          <View style={styles.barMid} />
          <View style={styles.barHot} />
          <View style={armed ? styles.barPeak : styles.barDim} />
        </View>
        <View style={styles.readoutBlock}>
          <Text style={styles.readoutLabel}>Output gain</Text>
          <Text style={styles.readout}>{gainDb > 0 ? '+' : ''}{gainDb.toFixed(1)} dB</Text>
          <Text style={styles.status}>{armed ? 'Signal path active' : 'Signal path muted'}</Text>
        </View>
      </View>

      <View style={styles.controls}>
        <Pressable accessibilityLabel="Decrease gain" onPress={decreaseGain} style={styles.stepButton}>
          <Text style={styles.stepText}>-</Text>
        </Pressable>
        <View style={styles.scale}>
          <View style={styles.scaleTrack}>
            <View style={styles.scaleFill} />
          </View>
          <View style={styles.scaleTicks}>
            <Text style={styles.tickText}>-24</Text>
            <Text style={styles.tickText}>0</Text>
            <Text style={styles.tickText}>+12</Text>
          </View>
        </View>
        <Pressable accessibilityLabel="Increase gain" onPress={increaseGain} style={styles.stepButton}>
          <Text style={styles.stepText}>+</Text>
        </Pressable>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: {
    width: 520,
    minHeight: 360,
    padding: 22,
    backgroundColor: '#111827',
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#2f3b52',
    gap: 20,
  },
  header: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 18,
  },
  titleBlock: {
    gap: 4,
  },
  eyebrow: {
    color: '#8fb3ff',
    fontSize: 12,
    fontWeight: '600',
  },
  title: {
    color: '#f8fafc',
    fontSize: 28,
    fontWeight: '700',
  },
  bypassButton: {
    minWidth: 92,
    minHeight: 38,
    paddingLeft: 16,
    paddingRight: 16,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: '#14532d',
    borderRadius: 6,
  },
  bypassText: {
    color: '#dcfce7',
    fontSize: 13,
    fontWeight: '700',
  },
  meterRow: {
    flexDirection: 'row',
    alignItems: 'stretch',
    gap: 18,
  },
  meterStack: {
    width: 96,
    minHeight: 168,
    padding: 10,
    justifyContent: 'flex-end',
    gap: 8,
    backgroundColor: '#060913',
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#233047',
  },
  barDim: {
    height: 18,
    borderRadius: 4,
    backgroundColor: '#1f2937',
  },
  barLow: {
    height: 18,
    borderRadius: 4,
    backgroundColor: '#10b981',
  },
  barMid: {
    height: 18,
    borderRadius: 4,
    backgroundColor: '#34d399',
  },
  barHot: {
    height: 18,
    borderRadius: 4,
    backgroundColor: '#f59e0b',
  },
  barPeak: {
    height: 18,
    borderRadius: 4,
    backgroundColor: '#ef4444',
  },
  readoutBlock: {
    flex: 1,
    minHeight: 168,
    padding: 18,
    justifyContent: 'center',
    backgroundColor: '#172033',
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#2f3b52',
    gap: 10,
  },
  readoutLabel: {
    color: '#a7b4ca',
    fontSize: 13,
    fontWeight: '600',
  },
  readout: {
    color: '#ffffff',
    fontSize: 42,
    fontWeight: '700',
  },
  status: {
    color: '#cbd5e1',
    fontSize: 14,
  },
  controls: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
  },
  stepButton: {
    width: 44,
    height: 44,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: '#2563eb',
    borderRadius: 6,
  },
  stepText: {
    color: '#eff6ff',
    fontSize: 26,
    fontWeight: '700',
  },
  scale: {
    flex: 1,
    gap: 8,
  },
  scaleTrack: {
    height: 12,
    backgroundColor: '#334155',
    borderRadius: 6,
  },
  scaleFill: {
    width: 258,
    height: 12,
    backgroundColor: '#60a5fa',
    borderRadius: 6,
  },
  scaleTicks: {
    flexDirection: 'row',
    justifyContent: 'space-between',
  },
  tickText: {
    color: '#94a3b8',
    fontSize: 12,
  },
});
